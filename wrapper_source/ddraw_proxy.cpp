#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define DIRECTDRAW_VERSION 0x0700
#define DIRECT3D_VERSION 0x0700
#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "IniReader.h"
#include "ts2fix/config.h"
#include "ts2fix/frame_timer_install.h"
#include "ts2fix/logging.h"

namespace
{
struct ModernDepthConfig
{
	bool enabled = true;
	bool reversedZ = false;
	bool dynamicNear = true;
	float nearMin = 1.0f;
	float nearMax = 300.0f;
	float farPlane = 20000.0f;
	std::string depthFormat = "auto";
	bool debugOverlay = false;
};

HMODULE g_module = nullptr;
HMODULE g_realDdraw = nullptr;

using DirectDrawCreateFn = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);
using DirectDrawCreateExFn = HRESULT(WINAPI*)(GUID*, LPVOID*, REFIID, IUnknown*);
using DirectDrawEnumerateAFn = HRESULT(WINAPI*)(LPDDENUMCALLBACKA, LPVOID);

DirectDrawCreateFn g_realDirectDrawCreate = nullptr;
DirectDrawCreateExFn g_realDirectDrawCreateEx = nullptr;
DirectDrawEnumerateAFn g_realDirectDrawEnumerateA = nullptr;

ModernDepthConfig g_config = {};
std::once_flag g_initOnce;
std::once_flag g_wrapperTimingOnce;

struct HookTable
{
	std::unordered_map<void**, void*> originals;
	std::mutex mutex;
};

HookTable g_queryInterfaceHooks = {};
HookTable g_createSurfaceHooks = {};
HookTable g_createSurface2Hooks = {};
HookTable g_createDevice2Hooks = {};
HookTable g_createDevice3Hooks = {};
HookTable g_setRenderStateHooks = {};
HookTable g_setTransformHooks = {};

constexpr std::size_t kVtableIndexQueryInterface = 0;
constexpr std::size_t kVtableIndexCreateSurface = 6;
constexpr std::size_t kVtableIndexCreateDevice = 8;
constexpr std::size_t kVtableIndexSetRenderState = 21;
constexpr std::size_t kVtableIndexSetTransform = 24;
constexpr float kDefaultDepthRatio = 20000.0f;

using QueryInterfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**);
using CreateSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(void*, DDSURFACEDESC*, void**, IUnknown*);
using CreateSurface2Fn = HRESULT(STDMETHODCALLTYPE*)(void*, DDSURFACEDESC2*, void**, IUnknown*);
using CreateDevice2Fn = HRESULT(STDMETHODCALLTYPE*)(void*, REFCLSID, IDirectDrawSurface*, void**);
using CreateDevice3Fn = HRESULT(STDMETHODCALLTYPE*)(void*, REFCLSID, IDirectDrawSurface4*, void**, IUnknown*);
using SetRenderStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DRENDERSTATETYPE, DWORD);
using SetTransformFn = HRESULT(STDMETHODCALLTYPE*)(void*, D3DTRANSFORMSTATETYPE, D3DMATRIX*);

bool FileExists(const std::string& path)
{
	const DWORD attrs = GetFileAttributesA(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string ToLower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

void Log(const char* fmt, ...)
{
	char body[768] = {};
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(body, sizeof(body), fmt, args);
	va_end(args);
	ts2fix::Log("ModernDepth", "%s", body);
}

std::string GetModuleDirectory()
{
	char modulePath[MAX_PATH] = {};
	if (g_module == nullptr || GetModuleFileNameA(g_module, modulePath, MAX_PATH) == 0)
		return {};

	std::string path = modulePath;
	const std::size_t slash = path.find_last_of("\\/");
	if (slash == std::string::npos)
		return {};
	return path.substr(0, slash + 1);
}

std::string ResolveIniPath()
{
	const std::string moduleDir = GetModuleDirectory();
	if (moduleDir.empty())
		return {};

	const std::string scriptsIni = moduleDir + "scripts\\ToyStory2Fix.ini";
	if (FileExists(scriptsIni))
		return scriptsIni;

	const std::string rootIni = moduleDir + "ToyStory2Fix.ini";
	if (FileExists(rootIni))
		return rootIni;

	return scriptsIni;
}

bool ParseBoolean(const std::string& value, bool defaultValue)
{
	const std::string lower = ToLower(value);
	if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
		return true;
	if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
		return false;
	return defaultValue;
}

bool ReadIniBoolean(const std::string& iniPath, const char* section, const char* key, bool defaultValue)
{
	char buffer[64] = {};
	GetPrivateProfileStringA(section, key, defaultValue ? "true" : "false", buffer, static_cast<DWORD>(sizeof(buffer)), iniPath.c_str());
	return ParseBoolean(buffer, defaultValue);
}

float ReadIniFloat(const std::string& iniPath, const char* section, const char* key, float defaultValue)
{
	char buffer[64] = {};
	std::snprintf(buffer, sizeof(buffer), "%.3f", defaultValue);
	GetPrivateProfileStringA(section, key, buffer, buffer, static_cast<DWORD>(sizeof(buffer)), iniPath.c_str());

	char* end = nullptr;
	const float value = std::strtof(buffer, &end);
	if (end == buffer || !std::isfinite(value))
		return defaultValue;
	return value;
}

std::string ReadIniString(const std::string& iniPath, const char* section, const char* key, const char* defaultValue)
{
	char buffer[64] = {};
	GetPrivateProfileStringA(section, key, defaultValue, buffer, static_cast<DWORD>(sizeof(buffer)), iniPath.c_str());
	return buffer;
}

void LoadConfig()
{
	const std::string iniPath = ResolveIniPath();
	g_config.enabled = ReadIniBoolean(iniPath, "Rendering", "modern_depth_pipeline", true);
	g_config.reversedZ = ReadIniBoolean(iniPath, "Rendering", "modern_depth_reversed_z", false);
	g_config.dynamicNear = ReadIniBoolean(iniPath, "Rendering", "modern_depth_dynamic_near", true);
	g_config.nearMin = std::max(0.1f, ReadIniFloat(iniPath, "Rendering", "modern_depth_near_min", 1.0f));
	g_config.nearMax = std::max(g_config.nearMin, ReadIniFloat(iniPath, "Rendering", "modern_depth_near_max", 300.0f));
	g_config.farPlane = std::max(g_config.nearMin + 1.0f, ReadIniFloat(iniPath, "Rendering", "modern_depth_far", 20000.0f));
	g_config.depthFormat = ToLower(ReadIniString(iniPath, "Rendering", "modern_depth_format", "auto"));
	g_config.debugOverlay = ReadIniBoolean(iniPath, "Rendering", "modern_depth_debug_overlay", false);

	Log("Config enabled=%d reversed_z=%d dynamic_near=%d near=[%.2f, %.2f] far=%.2f depth_format=%s\n",
		g_config.enabled ? 1 : 0,
		g_config.reversedZ ? 1 : 0,
		g_config.dynamicNear ? 1 : 0,
		g_config.nearMin,
		g_config.nearMax,
		g_config.farPlane,
		g_config.depthFormat.c_str());
}

void InitializeRealDdraw()
{
	char systemDir[MAX_PATH] = {};
	if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0)
		return;

	std::string ddrawPath = std::string(systemDir) + "\\ddraw.dll";
	g_realDdraw = LoadLibraryA(ddrawPath.c_str());
	if (g_realDdraw == nullptr)
	{
		Log("Failed to load real ddraw.dll from %s (err=%lu)\n", ddrawPath.c_str(), GetLastError());
		return;
	}

	g_realDirectDrawCreate = reinterpret_cast<DirectDrawCreateFn>(GetProcAddress(g_realDdraw, "DirectDrawCreate"));
	g_realDirectDrawCreateEx = reinterpret_cast<DirectDrawCreateExFn>(GetProcAddress(g_realDdraw, "DirectDrawCreateEx"));
	g_realDirectDrawEnumerateA = reinterpret_cast<DirectDrawEnumerateAFn>(GetProcAddress(g_realDdraw, "DirectDrawEnumerateA"));

	LoadConfig();
}

void EnsureInitialized()
{
	std::call_once(g_initOnce, InitializeRealDdraw);
}

void InitializeWrapperTimingPipeline()
{
	CIniReader iniReader("scripts\\ToyStory2Fix.ini");
	ts2fix::Config config = ts2fix::LoadConfig(iniReader);
	ts2fix::SetDiagnosticsEnabled(config.framerate.diagnostics);

	if (!config.framerate.enabled)
	{
		Log("Wrapper timing pipeline disabled in config.\n");
		return;
	}

	const bool installed = ts2fix::InstallFrameTimerHooks(config);
	Log("Wrapper timing pipeline install %s.\n", installed ? "succeeded" : "failed");
}

void EnsureWrapperTimingInitialized()
{
	std::call_once(g_wrapperTimingOnce, InitializeWrapperTimingPipeline);
}

bool PatchVtableEntry(void** slotAddress, void* detourAddress)
{
	DWORD oldProtect = 0;
	if (!VirtualProtect(slotAddress, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;

	*slotAddress = detourAddress;
	DWORD ignored = 0;
	VirtualProtect(slotAddress, sizeof(void*), oldProtect, &ignored);
	FlushInstructionCache(GetCurrentProcess(), slotAddress, sizeof(void*));
	return true;
}

template<typename T>
T GetOriginal(HookTable& table, void* self, std::size_t vtableIndex)
{
	if (self == nullptr)
		return nullptr;

	auto** vtable = *reinterpret_cast<void***>(self);
	if (vtable == nullptr)
		return nullptr;

	void** slotAddress = &vtable[vtableIndex];
	std::lock_guard<std::mutex> lock(table.mutex);
	const auto it = table.originals.find(slotAddress);
	if (it == table.originals.end())
		return nullptr;
	return reinterpret_cast<T>(it->second);
}

template<typename T>
bool HookMethod(HookTable& table, void* object, std::size_t vtableIndex, T detour, const char* name)
{
	if (object == nullptr)
		return false;

	auto** vtable = *reinterpret_cast<void***>(object);
	if (vtable == nullptr)
		return false;

	void** slotAddress = &vtable[vtableIndex];
	{
		std::lock_guard<std::mutex> lock(table.mutex);
		if (table.originals.find(slotAddress) == table.originals.end())
			table.originals.emplace(slotAddress, vtable[vtableIndex]);
		else if (vtable[vtableIndex] == reinterpret_cast<void*>(detour))
			return true;
	}

	if (!PatchVtableEntry(slotAddress, reinterpret_cast<void*>(detour)))
	{
		Log("Failed to patch %s at slot %zu (err=%lu)\n", name, vtableIndex, GetLastError());
		return false;
	}

	Log("Hooked %s slot=%zu\n", name, vtableIndex);
	return true;
}

void CollectDepthCandidates(std::vector<int>& candidates)
{
	const std::string mode = ToLower(g_config.depthFormat);
	if (mode == "d16")
	{
		candidates.push_back(16);
		return;
	}

	if (mode == "d24s8" || mode == "d24x8" || mode == "d24")
	{
		candidates.push_back(24);
		candidates.push_back(16);
		return;
	}

	if (mode == "d32f" || mode == "d32")
	{
		candidates.push_back(32);
		candidates.push_back(24);
		candidates.push_back(16);
		return;
	}

	// Auto mode prefers higher precision first, then degrades.
	candidates.push_back(32);
	candidates.push_back(24);
	candidates.push_back(16);
}

bool IsZBufferSurface(const DDSURFACEDESC* desc)
{
	if (desc == nullptr)
		return false;

	const bool capZ = (desc->dwFlags & DDSD_CAPS) != 0 && (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) != 0;
	const bool formatZ = (desc->dwFlags & DDSD_PIXELFORMAT) != 0 && (desc->ddpfPixelFormat.dwFlags & DDPF_ZBUFFER) != 0;
	return capZ || formatZ;
}

bool IsZBufferSurface(const DDSURFACEDESC2* desc)
{
	if (desc == nullptr)
		return false;

	const bool capZ = (desc->dwFlags & DDSD_CAPS) != 0 && (desc->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) != 0;
	const bool formatZ = (desc->dwFlags & DDSD_PIXELFORMAT) != 0 && (desc->ddpfPixelFormat.dwFlags & DDPF_ZBUFFER) != 0;
	return capZ || formatZ;
}

void ForceDepthFormat(DDSURFACEDESC& desc, int bits)
{
	desc.dwFlags |= DDSD_ZBUFFERBITDEPTH | DDSD_PIXELFORMAT;
	desc.dwZBufferBitDepth = static_cast<DWORD>(bits);
	desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	desc.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
	desc.ddpfPixelFormat.dwZBufferBitDepth = static_cast<DWORD>(bits);
}

void ForceDepthFormat(DDSURFACEDESC2& desc, int bits)
{
	desc.dwFlags |= DDSD_DEPTH | DDSD_PIXELFORMAT;
	desc.dwDepth = static_cast<DWORD>(bits);
	desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
	desc.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
	desc.ddpfPixelFormat.dwZBufferBitDepth = static_cast<DWORD>(bits);
}

bool DecodeProjectionNearFar(const D3DMATRIX& matrix, float& outNear, float& outFar)
{
	const float a = matrix._33;
	const float b = matrix._43;

	if (!std::isfinite(a) || !std::isfinite(b))
		return false;
	if (std::fabs(a) < 1e-6f || std::fabs(a - 1.0f) < 1e-6f)
		return false;

	const float nearPlane = -b / a;
	const float farPlane = -b / (a - 1.0f);
	if (!std::isfinite(nearPlane) || !std::isfinite(farPlane))
		return false;
	if (nearPlane <= 0.0f || farPlane <= nearPlane)
		return false;

	outNear = nearPlane;
	outFar = farPlane;
	return true;
}

void ApplyDepthProjectionPolicy(D3DMATRIX& matrix)
{
	// Expecting D3D-style projection matrix with clip-space w in _34.
	if (std::fabs(matrix._34 - 1.0f) > 0.2f || std::fabs(matrix._44) > 0.2f)
		return;

	float nearPlane = 0.0f;
	float farPlane = 0.0f;
	if (!DecodeProjectionNearFar(matrix, nearPlane, farPlane))
		return;

	float patchedFar = std::min(farPlane, g_config.farPlane);
	if (patchedFar <= nearPlane + 1.0f)
		patchedFar = nearPlane + 1.0f;

	float patchedNear = nearPlane;
	if (g_config.dynamicNear)
	{
		const float nearFromRatio = patchedFar / kDefaultDepthRatio;
		patchedNear = std::max(patchedNear, nearFromRatio);
	}

	patchedNear = std::clamp(patchedNear, g_config.nearMin, g_config.nearMax);
	if (patchedNear >= patchedFar - 1.0f)
		patchedNear = std::max(0.1f, patchedFar - 1.0f);

	if (g_config.reversedZ)
	{
		matrix._33 = patchedNear / (patchedNear - patchedFar);
		matrix._43 = (-patchedNear * patchedFar) / (patchedNear - patchedFar);
	}
	else
	{
		matrix._33 = patchedFar / (patchedFar - patchedNear);
		matrix._43 = (-patchedNear * patchedFar) / (patchedFar - patchedNear);
	}
}

void HookInterfaceByIid(void* object, REFIID iid);
void HookDevice(void* deviceObject);

HRESULT STDMETHODCALLTYPE QueryInterfaceHook(void* self, REFIID riid, void** object)
{
	auto original = GetOriginal<QueryInterfaceFn>(g_queryInterfaceHooks, self, kVtableIndexQueryInterface);
	if (original == nullptr)
		return E_FAIL;

	const HRESULT hr = original(self, riid, object);
	if (SUCCEEDED(hr) && object != nullptr && *object != nullptr)
		HookInterfaceByIid(*object, riid);
	return hr;
}

HRESULT STDMETHODCALLTYPE CreateSurfaceHook(void* self, DDSURFACEDESC* surfaceDesc, void** surface, IUnknown* outer)
{
	auto original = GetOriginal<CreateSurfaceFn>(g_createSurfaceHooks, self, kVtableIndexCreateSurface);
	if (original == nullptr || surfaceDesc == nullptr || !g_config.enabled || !IsZBufferSurface(surfaceDesc))
		return original ? original(self, surfaceDesc, surface, outer) : E_FAIL;

	std::vector<int> candidates;
	CollectDepthCandidates(candidates);
	for (const int bits : candidates)
	{
		DDSURFACEDESC patched = *surfaceDesc;
		ForceDepthFormat(patched, bits);
		const HRESULT hr = original(self, &patched, surface, outer);
		if (SUCCEEDED(hr))
		{
			Log("CreateSurface zbuffer request promoted to %d-bit.\n", bits);
			return hr;
		}
	}

	return original(self, surfaceDesc, surface, outer);
}

HRESULT STDMETHODCALLTYPE CreateSurface2Hook(void* self, DDSURFACEDESC2* surfaceDesc, void** surface, IUnknown* outer)
{
	auto original = GetOriginal<CreateSurface2Fn>(g_createSurface2Hooks, self, kVtableIndexCreateSurface);
	if (original == nullptr || surfaceDesc == nullptr || !g_config.enabled || !IsZBufferSurface(surfaceDesc))
		return original ? original(self, surfaceDesc, surface, outer) : E_FAIL;

	std::vector<int> candidates;
	CollectDepthCandidates(candidates);
	for (const int bits : candidates)
	{
		DDSURFACEDESC2 patched = *surfaceDesc;
		ForceDepthFormat(patched, bits);
		const HRESULT hr = original(self, &patched, surface, outer);
		if (SUCCEEDED(hr))
		{
			Log("CreateSurface2 zbuffer request promoted to %d-bit.\n", bits);
			return hr;
		}
	}

	return original(self, surfaceDesc, surface, outer);
}

HRESULT STDMETHODCALLTYPE CreateDevice2Hook(void* self, REFCLSID rclsid, IDirectDrawSurface* surface, void** device)
{
	auto original = GetOriginal<CreateDevice2Fn>(g_createDevice2Hooks, self, kVtableIndexCreateDevice);
	if (original == nullptr)
		return E_FAIL;

	const HRESULT hr = original(self, rclsid, surface, device);
	if (SUCCEEDED(hr) && device != nullptr && *device != nullptr)
		HookDevice(*device);
	return hr;
}

HRESULT STDMETHODCALLTYPE CreateDevice3Hook(void* self, REFCLSID rclsid, IDirectDrawSurface4* surface, void** device, IUnknown* outer)
{
	auto original = GetOriginal<CreateDevice3Fn>(g_createDevice3Hooks, self, kVtableIndexCreateDevice);
	if (original == nullptr)
		return E_FAIL;

	const HRESULT hr = original(self, rclsid, surface, device, outer);
	if (SUCCEEDED(hr) && device != nullptr && *device != nullptr)
		HookDevice(*device);
	return hr;
}

HRESULT STDMETHODCALLTYPE SetRenderStateHook(void* self, D3DRENDERSTATETYPE state, DWORD value)
{
	auto original = GetOriginal<SetRenderStateFn>(g_setRenderStateHooks, self, kVtableIndexSetRenderState);
	if (original == nullptr)
		return E_FAIL;

	DWORD patchedValue = value;
	if (g_config.enabled)
	{
		if (state == D3DRENDERSTATE_ZENABLE || state == D3DRENDERSTATE_ZWRITEENABLE)
			patchedValue = TRUE;
		else if (state == D3DRENDERSTATE_ZFUNC)
			patchedValue = g_config.reversedZ ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL;
	}

	return original(self, state, patchedValue);
}

HRESULT STDMETHODCALLTYPE SetTransformHook(void* self, D3DTRANSFORMSTATETYPE state, D3DMATRIX* matrix)
{
	auto original = GetOriginal<SetTransformFn>(g_setTransformHooks, self, kVtableIndexSetTransform);
	if (original == nullptr)
		return E_FAIL;

	if (!g_config.enabled || state != D3DTRANSFORMSTATE_PROJECTION || matrix == nullptr)
		return original(self, state, matrix);

	D3DMATRIX patched = *matrix;
	ApplyDepthProjectionPolicy(patched);
	return original(self, state, &patched);
}

void HookDirectDrawInterface(void* directDrawObject, bool desc2Surface)
{
	HookMethod(g_queryInterfaceHooks, directDrawObject, kVtableIndexQueryInterface, QueryInterfaceHook, "DirectDraw::QueryInterface");
	if (desc2Surface)
		HookMethod(g_createSurface2Hooks, directDrawObject, kVtableIndexCreateSurface, CreateSurface2Hook, "DirectDraw::CreateSurface2");
	else
		HookMethod(g_createSurfaceHooks, directDrawObject, kVtableIndexCreateSurface, CreateSurfaceHook, "DirectDraw::CreateSurface");
}

void HookD3D2Interface(void* d3dObject)
{
	HookMethod(g_queryInterfaceHooks, d3dObject, kVtableIndexQueryInterface, QueryInterfaceHook, "IDirect3D2::QueryInterface");
	HookMethod(g_createDevice2Hooks, d3dObject, kVtableIndexCreateDevice, CreateDevice2Hook, "IDirect3D2::CreateDevice");
}

void HookD3D3Interface(void* d3dObject)
{
	HookMethod(g_queryInterfaceHooks, d3dObject, kVtableIndexQueryInterface, QueryInterfaceHook, "IDirect3D3::QueryInterface");
	HookMethod(g_createDevice3Hooks, d3dObject, kVtableIndexCreateDevice, CreateDevice3Hook, "IDirect3D3::CreateDevice");
}

void HookDevice(void* deviceObject)
{
	HookMethod(g_queryInterfaceHooks, deviceObject, kVtableIndexQueryInterface, QueryInterfaceHook, "IDirect3DDevice::QueryInterface");
	HookMethod(g_setRenderStateHooks, deviceObject, kVtableIndexSetRenderState, SetRenderStateHook, "IDirect3DDevice::SetRenderState");
	HookMethod(g_setTransformHooks, deviceObject, kVtableIndexSetTransform, SetTransformHook, "IDirect3DDevice::SetTransform");
}

void HookInterfaceByIid(void* object, REFIID iid)
{
	if (object == nullptr)
		return;

	// Newer Windows SDKs don't expose IID_IDirectDraw3; IDirectDraw3 isn't used by TS2 anyway.
	if (InlineIsEqualGUID(iid, IID_IDirectDraw) || InlineIsEqualGUID(iid, IID_IDirectDraw2))
	{
		HookDirectDrawInterface(object, false);
		return;
	}

	if (InlineIsEqualGUID(iid, IID_IDirectDraw4) || InlineIsEqualGUID(iid, IID_IDirectDraw7))
	{
		HookDirectDrawInterface(object, true);
		return;
	}

	if (InlineIsEqualGUID(iid, IID_IDirect3D2))
	{
		HookD3D2Interface(object);
		return;
	}

	if (InlineIsEqualGUID(iid, IID_IDirect3D3))
	{
		HookD3D3Interface(object);
		return;
	}

	if (InlineIsEqualGUID(iid, IID_IDirect3D))
	{
		HookMethod(g_queryInterfaceHooks, object, kVtableIndexQueryInterface, QueryInterfaceHook, "IDirect3D::QueryInterface");
		return;
	}

	if (InlineIsEqualGUID(iid, IID_IDirect3DDevice2) || InlineIsEqualGUID(iid, IID_IDirect3DDevice3))
	{
		HookDevice(object);
	}
}
} // namespace

extern "C" __declspec(dllexport) BOOL WINAPI TS2ModernDepthProxyMarker()
{
	return TRUE;
}

// Don't define DirectDrawCreate* by those exact names; the Windows SDK declares them in <ddraw.h>.
// We export the expected names via wrapper_source/ddraw_proxy.def to avoid C/C++ linkage conflicts.
extern "C" HRESULT WINAPI TS2Fix_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* directDraw, IUnknown* outer)
{
	EnsureInitialized();
	EnsureWrapperTimingInitialized();
	if (g_realDirectDrawCreate == nullptr)
		return DDERR_GENERIC;

	const HRESULT hr = g_realDirectDrawCreate(guid, directDraw, outer);
	if (SUCCEEDED(hr) && directDraw != nullptr && *directDraw != nullptr && g_config.enabled)
		HookDirectDrawInterface(*directDraw, false);
	return hr;
}

extern "C" HRESULT WINAPI TS2Fix_DirectDrawCreateEx(GUID* guid, LPVOID* directDraw, REFIID iid, IUnknown* outer)
{
	EnsureInitialized();
	EnsureWrapperTimingInitialized();
	if (g_realDirectDrawCreateEx == nullptr)
		return E_NOTIMPL;

	const HRESULT hr = g_realDirectDrawCreateEx(guid, directDraw, iid, outer);
	if (SUCCEEDED(hr) && directDraw != nullptr && *directDraw != nullptr && g_config.enabled)
		HookInterfaceByIid(*directDraw, iid);
	return hr;
}

extern "C" HRESULT WINAPI TS2Fix_DirectDrawEnumerateA(LPDDENUMCALLBACKA callback, LPVOID context)
{
	EnsureInitialized();
	if (g_realDirectDrawEnumerateA == nullptr)
		return DDERR_GENERIC;
	return g_realDirectDrawEnumerateA(callback, context);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = module;
		DisableThreadLibraryCalls(module);
	}
	return TRUE;
}
