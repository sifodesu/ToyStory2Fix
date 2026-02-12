#include "stdafx.h"
#include "ts2fix/init.h"

#include "ts2fix/config.h"
#include "ts2fix/frame_timer.h"
#include "ts2fix/logging.h"
#include "ts2fix/patches_misc.h"
#include "ts2fix/pattern_utils.h"
#include "ts2fix/zero_speed_safety.h"
#include "ts2fix/widescreen.h"
#include "ts2fix/zbuffer_fix.h"

#include <MMSystem.h>
#include <algorithm>

namespace
{
constexpr ULONGLONG kInitTimeoutMs = 10000;
constexpr DWORD kInitRetrySleepMs = 10;

void InstallFrameTimerHooks(const ts2fix::Config& config)
{
	if (!config.framerate.enabled)
		return;

	auto& runtime = ts2fix::GetRuntimeContext();
	ts2fix::FramerateConfig timerConfig = config.framerate;

	if (timeGetDevCaps(&runtime.timerCaps, sizeof(runtime.timerCaps)) != TIMERR_NOERROR || runtime.timerCaps.wPeriodMin == 0)
		runtime.timerCaps.wPeriodMin = 1;
	if (!QueryPerformanceFrequency(&runtime.performanceFrequency) || runtime.performanceFrequency.QuadPart == 0)
		runtime.performanceFrequency.QuadPart = 1;

	runtime.framerateInitTickMs = GetTickCount64();
	runtime.frameTimerTargetAddress = 0;
	runtime.gameplayFrameTimerReturnAddress = 0;
	runtime.frontendFrameTimerReturnAddress = 0;
	runtime.menuFrameTimerReturnAddress = 0;
	runtime.variables.speedMultiplier = nullptr;
	runtime.variables.isDemoMode = nullptr;
	ts2fix::SetFrameTimerCallsiteAddresses(0, 0, 0);

	runtime.targetRefreshRateOverride = static_cast<uint32_t>(std::max(timerConfig.targetRefreshRate, 0));
	runtime.refreshRateProbePending = false;
	if (timerConfig.nativeRefreshRate)
	{
		const uint32_t refreshRate = runtime.targetRefreshRateOverride > 0
			? runtime.targetRefreshRateOverride
			: ts2fix::GetDesktopRefreshRate();
		ts2fix::ApplyRefreshRate(refreshRate);
		runtime.refreshRateProbePending = (runtime.targetRefreshRateOverride == 0);
	}
	else
	{
		ts2fix::ApplyRefreshRate(60);
	}

	auto pattern = hook::pattern("8B 0D ? ? ? ? 2B F1 3B");
	if (!pattern.count_hint(1).empty())
		runtime.variables.speedMultiplier = *reinterpret_cast<uint32_t**>(pattern.get_first(2));
	else
		ts2fix::Log("Init", "speedMultiplier pointer pattern not found.\n");

	pattern = hook::pattern("39 3D ? ? ? ? 75 27");
	if (!pattern.count_hint(1).empty())
		runtime.variables.isDemoMode = *reinterpret_cast<bool**>(pattern.get_first(2));
	else
		ts2fix::Log("Init", "isDemoMode pointer pattern not found.\n");

	runtime.zeroSpeedSafetyReady = ts2fix::InstallZeroSpeedSafetyPatches();
	if (!runtime.zeroSpeedSafetyReady)
	{
		ts2fix::Log("Safety", "High-refresh simulation safety patches unavailable. Falling back to safer timing behavior.\n");
	}
	if (timerConfig.frontendZeroStep && !runtime.zeroSpeedSafetyReady)
	{
		timerConfig.frontendZeroStep = false;
		ts2fix::Log("Init", "frontend_zero_step disabled because safety patches were unavailable.\n");
	}

	ts2fix::ConfigureFrameTimer(timerConfig);

	pattern = hook::pattern("C7 05 ? ? ? ? 00 00 00 00 E8 ? ? ? ? E8 ? ? ? ? 33");
	if (!pattern.count_hint(1).empty())
	{
		auto* callInstruction = pattern.get_first<uint8_t>(10);
		runtime.menuFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
		runtime.frameTimerTargetAddress = ts2fix::ResolveRelativeCall(callInstruction);
		injector::MakeCALL(callInstruction, ts2fix::FrameTimerHook);
	}
	else
	{
		ts2fix::Log("FrameTimer", "Menu frame-timer patch pattern not found.\n");
	}

	if (runtime.frameTimerTargetAddress != 0)
	{
		auto timerCallsites = ts2fix::FindDirectCallsToTarget(runtime.frameTimerTargetAddress);
		for (auto* callInstruction : timerCallsites)
		{
			const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
			if (returnAddress == runtime.menuFrameTimerReturnAddress)
				continue;

			const int pushedArg = ts2fix::GetImmediatePushArgBeforeCall(callInstruction);
			if (pushedArg == 1 && runtime.gameplayFrameTimerReturnAddress == 0)
				runtime.gameplayFrameTimerReturnAddress = returnAddress;
			else if (pushedArg == 0 && runtime.frontendFrameTimerReturnAddress == 0)
				runtime.frontendFrameTimerReturnAddress = returnAddress;

			injector::MakeCALL(callInstruction, ts2fix::FrameTimerHook);
		}

		if (runtime.gameplayFrameTimerReturnAddress == 0)
		{
			pattern = hook::pattern("83 C4 08 6A 01 E8 ? ? ? ?");
			if (!pattern.count_hint(1).empty())
			{
				auto* callInstruction = pattern.get_first<uint8_t>(5);
				runtime.gameplayFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
				injector::MakeCALL(callInstruction, ts2fix::FrameTimerHook);
			}
		}

		if (runtime.frontendFrameTimerReturnAddress == 0)
		{
			pattern = hook::pattern("E8 ? ? ? ? 6A 00 E8 ? ? ? ? 6A 01 E8 ? ? ? ? 83 C4");
			if (!pattern.count_hint(1).empty())
			{
				auto* callInstruction = pattern.get_first<uint8_t>(7);
				runtime.frontendFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
				injector::MakeCALL(callInstruction, ts2fix::FrameTimerHook);
			}
		}

		ts2fix::SetFrameTimerCallsiteAddresses(
			runtime.gameplayFrameTimerReturnAddress,
			runtime.frontendFrameTimerReturnAddress,
			runtime.menuFrameTimerReturnAddress);

		ts2fix::Log("FrameTimer", "Callsites gameplay=%p frontend=%p menu=%p\n",
			reinterpret_cast<void*>(runtime.gameplayFrameTimerReturnAddress),
			reinterpret_cast<void*>(runtime.frontendFrameTimerReturnAddress),
			reinterpret_cast<void*>(runtime.menuFrameTimerReturnAddress));

		ts2fix::InitializeFrameTimerModes();
	}
	else
	{
		ts2fix::Log("FrameTimer", "Could not resolve frame-timer target. Legacy game timing will be used.\n");
	}
}
} // namespace

namespace ts2fix
{
RuntimeContext& GetRuntimeContext()
{
	static RuntimeContext runtime = {};
	return runtime;
}

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

	InstallFrameTimerHooks(config);
	ApplyMiscPatches(config);
	if (config.rendering.zBufferFix)
		InstallZBufferFixHook();

	if (config.rendering.widescreen)
		InstallWidescreenHook();

	return 0;
}
} // namespace ts2fix
