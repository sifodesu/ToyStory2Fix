#include "stdafx.h"
#include "ts2fix/frame_timer.h"
#include "ts2fix/logging.h"
#include "ts2fix/runtime.h"

#include <MMSystem.h>
#include <cstddef>
#include <intrin.h>

#pragma intrinsic(_ReturnAddress)

namespace
{
constexpr int kGameplayFrameTimeUs = 16667;

enum class FrameTimerCallsite : uint8_t
{
	Unknown = 0,
	Gameplay,
	Frontend,
	Menu,
	Count
};

enum class FrameTimerMode : uint8_t
{
	LegacyPassthrough = 0,
	CustomSafe60,
	CustomZeroStep
};

struct FrameTimerState
{
	FrameTimerMode mode = FrameTimerMode::LegacyPassthrough;
	LARGE_INTEGER previousTime = {};
	int64_t nextFrameDeadlineQpc = 0;
	int64_t simulationAccumulatorUs = 0;
	uint32_t consecutiveZeroFrames = 0;
	uint32_t framesSinceNonZero = 0;
	uint32_t modeSwitchCount = 0;
};

FrameTimerState g_frameTimerStates[static_cast<std::size_t>(FrameTimerCallsite::Count)] = {};

bool g_autoFallbackTo60 = true;
bool g_allowFrontendCustomTiming = false;
bool g_allowFrontendZeroStep = false;
uint32_t g_startupGuardMs = 5000;

const char* GetCallsiteName(FrameTimerCallsite callsite)
{
	switch (callsite)
	{
	case FrameTimerCallsite::Gameplay:
		return "Gameplay";
	case FrameTimerCallsite::Frontend:
		return "Frontend";
	case FrameTimerCallsite::Menu:
		return "Menu";
	default:
		return "Unknown";
	}
}

const char* GetModeName(FrameTimerMode mode)
{
	switch (mode)
	{
	case FrameTimerMode::LegacyPassthrough:
		return "LegacyPassthrough";
	case FrameTimerMode::CustomSafe60:
		return "CustomSafe60";
	case FrameTimerMode::CustomZeroStep:
		return "CustomZeroStep";
	default:
		return "Unknown";
	}
}

FrameTimerState& GetFrameTimerState(FrameTimerCallsite callsite)
{
	return g_frameTimerStates[static_cast<std::size_t>(callsite)];
}

FrameTimerCallsite GetFrameTimerCallsite(uintptr_t returnAddress)
{
	const auto& runtime = ts2fix::GetRuntimeContext();
	if (returnAddress == runtime.gameplayFrameTimerReturnAddress)
		return FrameTimerCallsite::Gameplay;
	if (returnAddress == runtime.frontendFrameTimerReturnAddress)
		return FrameTimerCallsite::Frontend;
	if (returnAddress == runtime.menuFrameTimerReturnAddress)
		return FrameTimerCallsite::Menu;
	return FrameTimerCallsite::Unknown;
}

void ResetFrameTimerState(FrameTimerState& state)
{
	state.previousTime.QuadPart = 0;
	state.nextFrameDeadlineQpc = 0;
	state.simulationAccumulatorUs = 0;
	state.consecutiveZeroFrames = 0;
	state.framesSinceNonZero = 0;
}

void SetFrameTimerMode(FrameTimerCallsite callsite, FrameTimerMode newMode, const char* reason)
{
	auto& state = GetFrameTimerState(callsite);
	if (state.mode == newMode)
		return;

	const FrameTimerMode oldMode = state.mode;
	state.mode = newMode;
	state.modeSwitchCount += 1;
	ResetFrameTimerState(state);

	ts2fix::Log("FrameTimer", "%s %s -> %s (%s)\n",
		GetCallsiteName(callsite), GetModeName(oldMode), GetModeName(newMode), reason);
}

int64_t QueryElapsedMicroseconds(
	const LARGE_INTEGER& previousTime,
	LARGE_INTEGER& currentTime,
	const LARGE_INTEGER& frequency)
{
	QueryPerformanceCounter(&currentTime);
	if (frequency.QuadPart == 0)
		return 0;

	int64_t elapsedUs = currentTime.QuadPart - previousTime.QuadPart;
	elapsedUs *= 1000000;
	elapsedUs /= frequency.QuadPart;
	return elapsedUs;
}

FrameTimerMode GetPreferredGameplayMode()
{
	const auto& runtime = ts2fix::GetRuntimeContext();
	if (runtime.zeroSpeedSafetyReady && runtime.targetFrameTimeUs < kGameplayFrameTimeUs)
		return FrameTimerMode::CustomZeroStep;
	return FrameTimerMode::CustomSafe60;
}

void HandleAnomalyFallback(
	FrameTimerCallsite callsite,
	FrameTimerState& state,
	bool isDemoMode,
	int speedMultiplier,
	int frameTimeUs)
{
	if (isDemoMode || state.mode != FrameTimerMode::CustomZeroStep)
	{
		state.consecutiveZeroFrames = 0;
		state.framesSinceNonZero = 0;
		return;
	}

	if (speedMultiplier == 0)
	{
		state.consecutiveZeroFrames += 1;
		state.framesSinceNonZero += 1;
	}
	else
	{
		state.consecutiveZeroFrames = 0;
		state.framesSinceNonZero = 0;
	}

	if (!g_autoFallbackTo60)
		return;

	uint32_t zeroFrameThreshold = 120;
	uint64_t zeroStepTimeThresholdUs = 2000000ULL;

	if (callsite != FrameTimerCallsite::Gameplay && ts2fix::IsStartupGuardActive())
	{
		zeroFrameThreshold = 24;
		zeroStepTimeThresholdUs = 400000ULL;
	}

	const uint64_t noStepElapsedUs =
		static_cast<uint64_t>(state.framesSinceNonZero) * static_cast<uint64_t>(std::max(frameTimeUs, 1));
	const bool tooManyZeroFrames = state.consecutiveZeroFrames >= zeroFrameThreshold;
	const bool noStepTooLong = noStepElapsedUs >= zeroStepTimeThresholdUs;

	if (!tooManyZeroFrames && !noStepTooLong)
		return;

	if (callsite == FrameTimerCallsite::Gameplay)
		SetFrameTimerMode(callsite, FrameTimerMode::CustomSafe60, "anomaly detected");
	else
		SetFrameTimerMode(callsite, FrameTimerMode::LegacyPassthrough, "anomaly detected");
}

BOOL CALLBACK FindProcessWindow(HWND hwnd, LPARAM lParam)
{
	DWORD processId = 0;
	GetWindowThreadProcessId(hwnd, &processId);

	if (processId == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr)
	{
		*reinterpret_cast<HWND*>(lParam) = hwnd;
		return FALSE;
	}

	return TRUE;
}

int RunCustomFrameTimer(FrameTimerCallsite callsite, FrameTimerState& state, bool allowZeroStepSimulation)
{
	auto& runtime = ts2fix::GetRuntimeContext();
	const UINT timerPeriod = runtime.timerCaps.wPeriodMin ? runtime.timerCaps.wPeriodMin : 1;
	const int frameTimeUs = std::max(runtime.targetFrameTimeUs, 1);
	const bool isDemoMode = *runtime.variables.isDemoMode;

	int effectiveFrameTimeUs = frameTimeUs;
	if (isDemoMode)
	{
		// Keep demo mode pacing tied to the original 60->30 behavior.
		effectiveFrameTimeUs = 16667;
	}
	else if (!allowZeroStepSimulation && frameTimeUs < kGameplayFrameTimeUs)
	{
		// If zero-step simulation is unavailable, keep simulation at safe 60 Hz pacing.
		effectiveFrameTimeUs = kGameplayFrameTimeUs;
	}

	timeBeginPeriod(timerPeriod);

	if (state.previousTime.QuadPart == 0)
		QueryPerformanceCounter(&state.previousTime);
	if (state.nextFrameDeadlineQpc == 0)
		state.nextFrameDeadlineQpc = state.previousTime.QuadPart;

	LARGE_INTEGER currentTime = {};
	int64_t elapsedUs = QueryElapsedMicroseconds(state.previousTime, currentTime, runtime.performanceFrequency);
	if (elapsedUs < 0)
		elapsedUs = 0;

	if (isDemoMode)
	{
		state.simulationAccumulatorUs = 0;
		runtime.framerateFactor = (static_cast<int>(elapsedUs) / kGameplayFrameTimeUs) + 1;
		if (runtime.framerateFactor < 2)
			runtime.framerateFactor = 2;
		runtime.framerateFactor = std::clamp(runtime.framerateFactor, 1, 3);
		*runtime.variables.speedMultiplier = static_cast<uint32_t>(runtime.framerateFactor);
		state.consecutiveZeroFrames = 0;
		state.framesSinceNonZero = 0;
	}
	else
	{
		int64_t simulationDeltaUs = elapsedUs;
		if (allowZeroStepSimulation && frameTimeUs < kGameplayFrameTimeUs)
		{
			const int64_t jitterAbsUs = (elapsedUs >= frameTimeUs) ? (elapsedUs - frameTimeUs) : (frameTimeUs - elapsedUs);
			const int64_t jitterToleranceUs = std::max<int64_t>(800, frameTimeUs / 3);
			if (jitterAbsUs <= jitterToleranceUs)
				simulationDeltaUs = frameTimeUs;
		}

		state.simulationAccumulatorUs += simulationDeltaUs;
		const int64_t maxAccumulatorUs = static_cast<int64_t>(kGameplayFrameTimeUs) * 16;
		if (state.simulationAccumulatorUs > maxAccumulatorUs)
			state.simulationAccumulatorUs = maxAccumulatorUs;

		const int desiredSteps = std::clamp(static_cast<int>(state.simulationAccumulatorUs / kGameplayFrameTimeUs), 0, 3);
		if (desiredSteps > 0)
			state.simulationAccumulatorUs -= static_cast<int64_t>(desiredSteps) * kGameplayFrameTimeUs;

		const int minSpeedMultiplier = allowZeroStepSimulation ? 0 : 1;
		runtime.framerateFactor = std::clamp(desiredSteps, minSpeedMultiplier, 3);

		if (runtime.framerateFactor > desiredSteps)
		{
			state.simulationAccumulatorUs -= static_cast<int64_t>(runtime.framerateFactor - desiredSteps) * kGameplayFrameTimeUs;
			if (state.simulationAccumulatorUs < 0)
				state.simulationAccumulatorUs = 0;
		}

		*runtime.variables.speedMultiplier = static_cast<uint32_t>(runtime.framerateFactor);
		HandleAnomalyFallback(callsite, state, false, runtime.framerateFactor, frameTimeUs);
	}

	const int pacingFactor = isDemoMode ? std::max(runtime.framerateFactor, 2) : 1;
	const int pacingFrameTimeUs = effectiveFrameTimeUs * pacingFactor;
	const int64_t frameDurationQpc =
		std::max<int64_t>(1, (static_cast<int64_t>(pacingFrameTimeUs) * runtime.performanceFrequency.QuadPart + 500000) / 1000000);

	int64_t targetDeadlineQpc = state.nextFrameDeadlineQpc + frameDurationQpc;
	const int64_t maxLagQpc = frameDurationQpc * 4;
	if (currentTime.QuadPart > targetDeadlineQpc + maxLagQpc)
		targetDeadlineQpc = currentTime.QuadPart + frameDurationQpc;

	const bool highRefreshPacing = pacingFrameTimeUs <= 10000;
	const int64_t sleepMarginUs = highRefreshPacing
		? std::max<int64_t>(3500, pacingFrameTimeUs / 2)
		: std::max<int64_t>(2000, pacingFrameTimeUs / 5);
	const int64_t yieldMarginUs = highRefreshPacing
		? std::max<int64_t>(1200, pacingFrameTimeUs / 6)
		: std::max<int64_t>(800, pacingFrameTimeUs / 10);
	const int64_t spinMarginUs = highRefreshPacing ? 300 : 150;

	runtime.sleepTime = 0;
	for (;;)
	{
		QueryPerformanceCounter(&currentTime);
		const int64_t remainingQpc = targetDeadlineQpc - currentTime.QuadPart;
		const int64_t remainingUs = (remainingQpc * 1000000) / runtime.performanceFrequency.QuadPart;
		if (remainingUs <= 0)
			break;

		if (remainingUs > sleepMarginUs)
		{
			const int64_t sleepCandidateUs = remainingUs - sleepMarginUs;
			const int candidateMs = static_cast<int>(sleepCandidateUs / 1000);
			if (candidateMs > 0)
			{
				runtime.sleepTime = candidateMs;
				Sleep(runtime.sleepTime);
				continue;
			}
		}

		if (remainingUs > yieldMarginUs)
			Sleep(0);
		else if (remainingUs > spinMarginUs)
			SwitchToThread();
		else
			YieldProcessor();
	}

	QueryPerformanceCounter(&currentTime);
	state.previousTime = currentTime;
	state.nextFrameDeadlineQpc = targetDeadlineQpc;
	timeEndPeriod(timerPeriod);

	return static_cast<int>(timeGetTime());
}
} // namespace

namespace ts2fix
{
void ConfigureFrameTimer(const FramerateConfig& config)
{
	g_autoFallbackTo60 = config.autoFallbackTo60;
	g_allowFrontendCustomTiming = config.frontendCustomTiming;
	g_allowFrontendZeroStep = config.frontendZeroStep;
	g_startupGuardMs = config.startupGuardMs;
}

void SetFrameTimerCallsiteAddresses(uintptr_t gameplayReturnAddress, uintptr_t frontendReturnAddress, uintptr_t menuReturnAddress)
{
	auto& runtime = GetRuntimeContext();
	runtime.gameplayFrameTimerReturnAddress = gameplayReturnAddress;
	runtime.frontendFrameTimerReturnAddress = frontendReturnAddress;
	runtime.menuFrameTimerReturnAddress = menuReturnAddress;
}

bool IsStartupGuardActive()
{
	const auto& runtime = GetRuntimeContext();
	if (g_startupGuardMs == 0 || runtime.framerateInitTickMs == 0)
		return false;
	return (GetTickCount64() - runtime.framerateInitTickMs) < g_startupGuardMs;
}

void InitializeFrameTimerModes()
{
	for (auto& state : g_frameTimerStates)
	{
		state.mode = FrameTimerMode::LegacyPassthrough;
		state.modeSwitchCount = 0;
		ResetFrameTimerState(state);
	}

	SetFrameTimerMode(FrameTimerCallsite::Gameplay, GetPreferredGameplayMode(), "initial setup");

	if (g_allowFrontendCustomTiming)
	{
		const auto& runtime = GetRuntimeContext();
		const FrameTimerMode frontendMode =
			(g_allowFrontendZeroStep && runtime.zeroSpeedSafetyReady) ? FrameTimerMode::CustomZeroStep : FrameTimerMode::CustomSafe60;
		SetFrameTimerMode(FrameTimerCallsite::Frontend, frontendMode, "initial setup");
		SetFrameTimerMode(FrameTimerCallsite::Menu, frontendMode, "initial setup");
	}
	else
	{
		LogDiagnostic("FrameTimer", "Frontend/Menu frame timer running in legacy mode.\n");
	}
}

void ApplyRefreshRate(uint32_t refreshRate)
{
	auto& runtime = GetRuntimeContext();
	if (refreshRate < 30 || refreshRate > 1000)
		refreshRate = 60;

	runtime.targetFrameTimeUs = (1000000 + (refreshRate / 2)) / refreshRate;
	for (auto& state : g_frameTimerStates)
		ResetFrameTimerState(state);

	if (runtime.gameplayFrameTimerReturnAddress != 0)
		SetFrameTimerMode(FrameTimerCallsite::Gameplay, GetPreferredGameplayMode(), "refresh update");
}

uint32_t GetDesktopRefreshRate()
{
	DEVMODEA displayMode = {};
	displayMode.dmSize = sizeof(displayMode);
	if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &displayMode) &&
		displayMode.dmDisplayFrequency > 1 &&
		displayMode.dmDisplayFrequency != 0xFFFFFFFFu)
	{
		return displayMode.dmDisplayFrequency;
	}

	return 60;
}

uint32_t GetProcessWindowRefreshRate()
{
	HWND processWindow = nullptr;
	EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&processWindow));
	if (processWindow == nullptr)
		return 0;

	HMONITOR monitor = MonitorFromWindow(processWindow, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXA monitorInfo = {};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!GetMonitorInfoA(monitor, &monitorInfo))
		return 0;

	DEVMODEA displayMode = {};
	displayMode.dmSize = sizeof(displayMode);
	if (EnumDisplaySettingsA(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode) &&
		displayMode.dmDisplayFrequency > 1 &&
		displayMode.dmDisplayFrequency != 0xFFFFFFFFu)
	{
		return displayMode.dmDisplayFrequency;
	}

	return 0;
}

int __cdecl FrameTimerHook(int a1)
{
	auto& runtime = GetRuntimeContext();
	auto original = reinterpret_cast<int(__cdecl*)(int)>(runtime.frameTimerTargetAddress);

	if (runtime.variables.speedMultiplier == nullptr || runtime.variables.isDemoMode == nullptr)
		return original ? original(a1) : 0;

	if (runtime.refreshRateProbePending)
	{
		const uint32_t processRefreshRate = GetProcessWindowRefreshRate();
		if (processRefreshRate > 0)
		{
			ApplyRefreshRate(processRefreshRate);
			runtime.refreshRateProbePending = false;
		}
	}

	const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
	const FrameTimerCallsite callsite = GetFrameTimerCallsite(returnAddress);
	if (callsite == FrameTimerCallsite::Unknown)
		return original ? original(a1) : 0;

	auto& state = GetFrameTimerState(callsite);
	if (IsStartupGuardActive())
		return original ? original(a1) : 0;
	if (state.mode == FrameTimerMode::LegacyPassthrough)
		return original ? original(a1) : 0;

	const bool allowZeroStepSimulation =
		(state.mode == FrameTimerMode::CustomZeroStep) &&
		runtime.zeroSpeedSafetyReady &&
		runtime.targetFrameTimeUs < kGameplayFrameTimeUs;

	LogDiagnostic("FrameTimer", "%s mode=%s a1=%d\n", GetCallsiteName(callsite), GetModeName(state.mode), a1);
	return RunCustomFrameTimer(callsite, state, allowZeroStepSimulation);
}
} // namespace ts2fix
