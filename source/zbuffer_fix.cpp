#include "stdafx.h"
#include "ts2fix/zbuffer_fix.h"

#include "ts2fix/logging.h"
#include "ts2fix/pattern_utils.h"

namespace
{
constexpr DWORD kRenderStateZEnable = 0x7;
constexpr DWORD kRenderStateZWriteEnable = 0xE;
constexpr DWORD kRenderStateZFunc = 0x17;
constexpr DWORD kDepthEnabled = 1;
constexpr DWORD kDepthFuncLessEqual = 4;
constexpr std::size_t kSetRenderStateVtableIndex = 0x5C / sizeof(void*);

uintptr_t g_applyRenderStatesTargetAddress = 0;
uintptr_t* g_devicePointerStorage = nullptr;
float g_patchedNearPlane = 100.0f;
float g_patchedFarPlane = 20000.0f;

using ApplyRenderStatesFn = int(*)();
using SetRenderStateFn = HRESULT(__stdcall*)(void* device, DWORD state, DWORD value);

void ForceDepthState()
{
	if (g_devicePointerStorage == nullptr || *g_devicePointerStorage == 0)
		return;

	void* device = reinterpret_cast<void*>(*g_devicePointerStorage);
	auto** vtable = *reinterpret_cast<void***>(device);
	if (vtable == nullptr)
		return;

	auto setRenderState = reinterpret_cast<SetRenderStateFn>(vtable[kSetRenderStateVtableIndex]);
	if (setRenderState == nullptr)
		return;

	setRenderState(device, kRenderStateZEnable, kDepthEnabled);
	setRenderState(device, kRenderStateZWriteEnable, kDepthEnabled);
	setRenderState(device, kRenderStateZFunc, kDepthFuncLessEqual);
}

int ApplyRenderStatesHook()
{
	int result = 1;
	if (g_applyRenderStatesTargetAddress != 0)
	{
		auto original = reinterpret_cast<ApplyRenderStatesFn>(g_applyRenderStatesTargetAddress);
		result = original();
	}

	ForceDepthState();
	return result;
}

bool PatchProjectionDepthRange()
{
	// This block initializes projection defaults:
	// [ptr+0x48] = 50.0f (near), [ptr+0x4C] = 32768.0f (far).
	auto pattern = hook::pattern(
		"C7 40 44 00 00 40 3F "
		"8B 0D ? ? ? ? "
		"C7 41 40 00 00 A0 3F "
		"8B 15 ? ? ? ? "
		"C7 42 4C 00 00 00 47 "
		"A1 ? ? ? ? "
		"C7 40 48 00 00 48 42");

	if (pattern.count_hint(1).empty())
	{
		Log("ZBuffer", "Projection depth-range pattern not found; precision patch skipped.\n");
		return false;
	}

	float* farImmediate = pattern.get_first<float>(29);
	float* nearImmediate = pattern.get_first<float>(41);
	injector::WriteMemory<float>(farImmediate, g_patchedFarPlane, true);
	injector::WriteMemory<float>(nearImmediate, g_patchedNearPlane, true);

	uintptr_t* projectionStorage = reinterpret_cast<uintptr_t*>(*pattern.get_first<uintptr_t>(34));
	if (projectionStorage != nullptr && *projectionStorage != 0)
	{
		float* projection = reinterpret_cast<float*>(*projectionStorage);
		projection[0x48 / sizeof(float)] = g_patchedNearPlane;
		projection[0x4C / sizeof(float)] = g_patchedFarPlane;
	}

	Log("ZBuffer", "Projection depth range patched near=%.1f far=%.1f.\n", g_patchedNearPlane, g_patchedFarPlane);
	return true;
}
} // namespace

namespace ts2fix
{
bool InstallZBufferFixHook(const RenderingConfig& renderingConfig)
{
	g_patchedNearPlane = renderingConfig.zBufferNearPlane;
	g_patchedFarPlane = renderingConfig.zBufferFarPlane;

	const bool projectionPatched = PatchProjectionDepthRange();

	// The depth-state function contains SetRenderState calls for ZENABLE (7), ZWRITEENABLE (14), and ZFUNC (23).
	auto pattern = hook::pattern("66 83 3D ? ? ? ? 02 56 8B 35 ? ? ? ? 0F 85 ? ? ? ? A1 ? ? ? ? 85 C0");
	if (pattern.count_hint(1).empty())
	{
		Log("ZBuffer", "Depth-state function pattern not found; z-buffer fix skipped.\n");
		return projectionPatched;
	}

	g_applyRenderStatesTargetAddress = reinterpret_cast<uintptr_t>(pattern.get_first(0));
	g_devicePointerStorage = *reinterpret_cast<uintptr_t**>(pattern.get_first(11));
	if (g_devicePointerStorage == nullptr)
	{
		Log("ZBuffer", "Device pointer storage was null; z-buffer fix skipped.\n");
		return projectionPatched;
	}

	auto callsites = FindDirectCallsToTarget(g_applyRenderStatesTargetAddress);
	if (callsites.empty())
	{
		Log("ZBuffer", "No depth-state callsites found; z-buffer fix skipped.\n");
		return projectionPatched;
	}

	for (auto* callInstruction : callsites)
		injector::MakeCALL(callInstruction, ApplyRenderStatesHook);

	Log("ZBuffer", "Installed depth-state hook at %p across %zu callsites.\n",
		reinterpret_cast<void*>(g_applyRenderStatesTargetAddress),
		callsites.size());

	ForceDepthState();
	return true;
}
} // namespace ts2fix
