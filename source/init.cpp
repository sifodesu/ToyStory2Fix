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

bool IsModernDepthWrapperActive()
{
	HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
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

	const bool wrapperActive = IsModernDepthWrapperActive();
	if (wrapperActive)
	{
		Log("Init", "Modern wrapper detected; high-fps timing is handled by ddraw wrapper.\n");
	}
	else
	{
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
