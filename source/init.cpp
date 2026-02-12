#include "stdafx.h"
#include "ts2fix/init.h"

#include "ts2fix/config.h"
#include "ts2fix/frame_timer_install.h"
#include "ts2fix/logging.h"
#include "ts2fix/patches_misc.h"
#include "ts2fix/widescreen.h"
#include "ts2fix/zbuffer_fix.h"

namespace
{
constexpr ULONGLONG kInitTimeoutMs = 10000;
constexpr DWORD kInitRetrySleepMs = 10;

std::string GetExeDirectory()
{
	char modulePath[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) == 0)
		return {};

	std::string path = modulePath;
	const std::size_t slash = path.find_last_of("\\/");
	if (slash == std::string::npos)
		return {};
	return path.substr(0, slash + 1);
}

bool FileExists(const std::string& path)
{
	if (path.empty())
		return false;
	const DWORD attrs = GetFileAttributesA(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string GetLoadedModulePath(HMODULE module)
{
	char path[MAX_PATH] = {};
	if (module == nullptr || GetModuleFileNameA(module, path, MAX_PATH) == 0)
		return {};
	return path;
}

HMODULE TryLoadLocalDdraw(DWORD* loadError)
{
	if (loadError != nullptr)
		*loadError = 0;

	const std::string exeDir = GetExeDirectory();
	if (exeDir.empty())
		return nullptr;

	const std::string localDdrawPath = exeDir + "ddraw.dll";
	if (!FileExists(localDdrawPath))
		return nullptr;

	HMODULE module = LoadLibraryA(localDdrawPath.c_str());
	if (module == nullptr && loadError != nullptr)
		*loadError = GetLastError();
	return module;
}

bool IsModernDepthWrapperActive(bool* attemptedLoad, DWORD* loadError, std::string* loadedDdrawPath)
{
	if (attemptedLoad != nullptr)
		*attemptedLoad = false;
	if (loadError != nullptr)
		*loadError = 0;
	if (loadedDdrawPath != nullptr)
		loadedDdrawPath->clear();

	HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
	if (ddrawModule == nullptr)
	{
		// The ASI can initialize before the game touches DirectDraw. If the wrapper is present on disk,
		// proactively load it so the "wrapper detected" path can be taken deterministically.
		if (attemptedLoad != nullptr)
			*attemptedLoad = true;
		ddrawModule = TryLoadLocalDdraw(loadError);
	}

	if (loadedDdrawPath != nullptr)
		*loadedDdrawPath = GetLoadedModulePath(ddrawModule);

	if (ddrawModule == nullptr)
		return false;

	return GetProcAddress(ddrawModule, "TS2ModernDepthProxyMarker") != nullptr;
}
} // namespace

namespace ts2fix
{
DWORD WINAPI Init(LPVOID delayed)
{
	auto pattern = hook::pattern("03 D1 2B D7 85 D2 7E 09 52 E8 ? ? ? ?");
	if (pattern.count_hint(1).empty() && delayed == nullptr)
	{
		CreateThread(0, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(&Init), reinterpret_cast<LPVOID>(1), 0, nullptr);
		return 0;
	}

	if (delayed != nullptr)
	{
		const ULONGLONG startMs = GetTickCount64();
		while (pattern.clear().count_hint(1).empty())
		{
			if (GetTickCount64() - startMs >= kInitTimeoutMs)
			{
				Log("Init", "Timeout waiting for target pattern.\n");
				return 0;
			}
			Sleep(kInitRetrySleepMs);
		}
	}

	CIniReader iniReader("ToyStory2Fix.ini");
	Config config = LoadConfig(iniReader);
	SetDiagnosticsEnabled(config.framerate.diagnostics);

	bool attemptedDdrawLoad = false;
	DWORD ddrawLoadError = 0;
	std::string loadedDdrawPath;
	const bool wrapperActive = IsModernDepthWrapperActive(&attemptedDdrawLoad, &ddrawLoadError, &loadedDdrawPath);
	if (wrapperActive)
	{
		Log("Init", "Modern wrapper detected; high-fps timing is handled by ddraw wrapper.\n");
	}
	else
	{
		// Provide breadcrumbs when users have a ddraw.dll on disk but it isn't active at runtime.
		if (config.rendering.modernDepthPipeline)
		{
			const std::string exeDir = GetExeDirectory();
			const std::string localDdrawPath = exeDir.empty() ? std::string{} : (exeDir + "ddraw.dll");
			const bool localDdrawExists = FileExists(localDdrawPath);

			if (!loadedDdrawPath.empty())
				Log("Init", "ddraw.dll loaded from: %s\n", loadedDdrawPath.c_str());
			else if (attemptedDdrawLoad && localDdrawExists)
				Log("Init", "ddraw.dll exists at %s but could not be loaded (err=%lu)\n", localDdrawPath.c_str(), ddrawLoadError);
			else if (localDdrawExists)
				Log("Init", "ddraw.dll exists at %s but was not loaded yet.\n", localDdrawPath.c_str());
			else if (!exeDir.empty())
				Log("Init", "No local ddraw.dll found at %s\n", localDdrawPath.c_str());
		}

		InstallFrameTimerHooks(config);
	}

	ApplyMiscPatches(config);

	const bool modernDepthWrapperActive = wrapperActive && config.rendering.modernDepthPipeline;
	if (config.rendering.modernDepthPipeline)
	{
		if (modernDepthWrapperActive)
			Log("Init", "Modern depth wrapper detected; legacy z-buffer patch disabled.\n");
		else
			Log("Init", "modern_depth_pipeline enabled but wrapper not detected; using legacy z-buffer patch.\n");
	}

	if (config.rendering.zBufferFix && !modernDepthWrapperActive)
		InstallZBufferFixHook(config.rendering);

	if (config.rendering.widescreen)
		InstallWidescreenHook();

	return 0;
}
} // namespace ts2fix
