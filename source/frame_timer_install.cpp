#include "stdafx.h"
#include "ts2fix/frame_timer_install.h"

#include "ts2fix/frame_timer.h"
#include "ts2fix/logging.h"
#include "ts2fix/pattern_utils.h"
#include "ts2fix/runtime.h"
#include "ts2fix/zero_speed_safety.h"

#include <MMSystem.h>
#include <algorithm>

namespace ts2fix
{
bool InstallFrameTimerHooks(const Config& config)
{
	if (!config.framerate.enabled)
		return false;

	auto& runtime = GetRuntimeContext();
	FramerateConfig timerConfig = config.framerate;

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
	SetFrameTimerCallsiteAddresses(0, 0, 0);

	runtime.targetRefreshRateOverride = static_cast<uint32_t>(std::max(timerConfig.targetRefreshRate, 0));
	runtime.refreshRateProbePending = false;
	if (timerConfig.nativeRefreshRate)
	{
		const uint32_t refreshRate = runtime.targetRefreshRateOverride > 0
			? runtime.targetRefreshRateOverride
			: GetDesktopRefreshRate();
		ApplyRefreshRate(refreshRate);
		runtime.refreshRateProbePending = (runtime.targetRefreshRateOverride == 0);
	}
	else
	{
		ApplyRefreshRate(60);
	}

	auto pattern = hook::pattern("8B 0D ? ? ? ? 2B F1 3B");
	if (!pattern.count_hint(1).empty())
		runtime.variables.speedMultiplier = *reinterpret_cast<uint32_t**>(pattern.get_first(2));
	else
		Log("Init", "speedMultiplier pointer pattern not found.\n");

	pattern = hook::pattern("39 3D ? ? ? ? 75 27");
	if (!pattern.count_hint(1).empty())
		runtime.variables.isDemoMode = *reinterpret_cast<bool**>(pattern.get_first(2));
	else
		Log("Init", "isDemoMode pointer pattern not found.\n");

	runtime.zeroSpeedSafetyReady = InstallZeroSpeedSafetyPatches();
	if (!runtime.zeroSpeedSafetyReady)
	{
		Log("Safety", "High-refresh simulation safety patches unavailable. Falling back to safer timing behavior.\n");
	}
	if (timerConfig.frontendZeroStep && !runtime.zeroSpeedSafetyReady)
	{
		timerConfig.frontendZeroStep = false;
		Log("Init", "frontend_zero_step disabled because safety patches were unavailable.\n");
	}

	ConfigureFrameTimer(timerConfig);

	pattern = hook::pattern("C7 05 ? ? ? ? 00 00 00 00 E8 ? ? ? ? E8 ? ? ? ? 33");
	if (!pattern.count_hint(1).empty())
	{
		auto* callInstruction = pattern.get_first<uint8_t>(10);
		runtime.menuFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
		runtime.frameTimerTargetAddress = ResolveRelativeCall(callInstruction);
		injector::MakeCALL(callInstruction, FrameTimerHook);
	}
	else
	{
		Log("FrameTimer", "Menu frame-timer patch pattern not found.\n");
	}

	if (runtime.frameTimerTargetAddress != 0)
	{
		auto timerCallsites = FindDirectCallsToTarget(runtime.frameTimerTargetAddress);
		for (auto* callInstruction : timerCallsites)
		{
			const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
			if (returnAddress == runtime.menuFrameTimerReturnAddress)
				continue;

			const int pushedArg = GetImmediatePushArgBeforeCall(callInstruction);
			if (pushedArg == 1 && runtime.gameplayFrameTimerReturnAddress == 0)
				runtime.gameplayFrameTimerReturnAddress = returnAddress;
			else if (pushedArg == 0 && runtime.frontendFrameTimerReturnAddress == 0)
				runtime.frontendFrameTimerReturnAddress = returnAddress;

			injector::MakeCALL(callInstruction, FrameTimerHook);
		}

		if (runtime.gameplayFrameTimerReturnAddress == 0)
		{
			pattern = hook::pattern("83 C4 08 6A 01 E8 ? ? ? ?");
			if (!pattern.count_hint(1).empty())
			{
				auto* callInstruction = pattern.get_first<uint8_t>(5);
				runtime.gameplayFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
				injector::MakeCALL(callInstruction, FrameTimerHook);
			}
		}

		if (runtime.frontendFrameTimerReturnAddress == 0)
		{
			pattern = hook::pattern("E8 ? ? ? ? 6A 00 E8 ? ? ? ? 6A 01 E8 ? ? ? ? 83 C4");
			if (!pattern.count_hint(1).empty())
			{
				auto* callInstruction = pattern.get_first<uint8_t>(7);
				runtime.frontendFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
				injector::MakeCALL(callInstruction, FrameTimerHook);
			}
		}

		SetFrameTimerCallsiteAddresses(
			runtime.gameplayFrameTimerReturnAddress,
			runtime.frontendFrameTimerReturnAddress,
			runtime.menuFrameTimerReturnAddress);

		Log("FrameTimer", "Callsites gameplay=%p frontend=%p menu=%p\n",
			reinterpret_cast<void*>(runtime.gameplayFrameTimerReturnAddress),
			reinterpret_cast<void*>(runtime.frontendFrameTimerReturnAddress),
			reinterpret_cast<void*>(runtime.menuFrameTimerReturnAddress));

		InitializeFrameTimerModes();
	}
	else
	{
		Log("FrameTimer", "Could not resolve frame-timer target. Legacy game timing will be used.\n");
	}

	return runtime.frameTimerTargetAddress != 0;
}
} // namespace ts2fix
