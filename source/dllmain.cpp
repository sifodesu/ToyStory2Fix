#include "stdafx.h"
#include <MMSystem.h>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <intrin.h>
#include <vector>

#pragma intrinsic(_ReturnAddress)

uintptr_t sub_490860_addr;
uintptr_t sub_49D910_addr;
TIMECAPS tc;
LARGE_INTEGER Frequency;
int sleepTime;
int framerateFactor;
int targetFrameTimeUs = 16667;
bool refreshRateProbePending = false;
bool zeroSpeedSafetyReady = false;
uint32_t targetRefreshRateOverride = 0;
uintptr_t gameplayFrameTimerReturnAddress = 0;
uintptr_t frontendFrameTimerReturnAddress = 0;
uintptr_t menuFrameTimerReturnAddress = 0;
bool framerateDiagnostics = false;
bool autoFallbackTo60 = true;
bool allowFrontendCustomTiming = false;
bool allowFrontendZeroStep = false;
uint32_t startupGuardMs = 5000;
ULONGLONG framerateInitTickMs = 0;

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

FrameTimerState frameTimerStates[static_cast<std::size_t>(FrameTimerCallsite::Count)];

uintptr_t ResolveRelativeCall(uint8_t* callInstruction)
{
	int32_t relativeTarget = *reinterpret_cast<int32_t*>(callInstruction + 1);
	return reinterpret_cast<uintptr_t>(callInstruction + 5 + relativeTarget);
}

std::vector<uint8_t*> FindDirectCallsToTarget(uintptr_t targetAddress)
{
	std::vector<uint8_t*> callInstructions;
	uint8_t* moduleBase = reinterpret_cast<uint8_t*>(GetModuleHandle(nullptr));
	if (moduleBase == nullptr)
		return callInstructions;

	auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		return callInstructions;

	auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(moduleBase + dosHeader->e_lfanew);
	if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
		return callInstructions;

	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeaders);
	for (uint16_t i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i, ++section)
	{
		if ((section->Characteristics & IMAGE_SCN_CNT_CODE) == 0)
			continue;

		const std::size_t sectionSize = static_cast<std::size_t>(section->Misc.VirtualSize);
		if (sectionSize < 5)
			continue;

		uint8_t* sectionStart = moduleBase + section->VirtualAddress;
		uint8_t* sectionEnd = sectionStart + sectionSize - 5;
		for (uint8_t* cursor = sectionStart; cursor <= sectionEnd; ++cursor)
		{
			if (*cursor != 0xE8)
				continue;

			int32_t relativeTarget = *reinterpret_cast<int32_t*>(cursor + 1);
			uintptr_t destination = reinterpret_cast<uintptr_t>(cursor + 5 + relativeTarget);
			if (destination == targetAddress)
				callInstructions.push_back(cursor);
		}
	}

	return callInstructions;
}

int GetImmediatePushArgBeforeCall(uint8_t* callInstruction)
{
	if (callInstruction == nullptr)
		return -1;

	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(callInstruction, &mbi, sizeof(mbi)) == 0)
		return -1;

	auto* regionStart = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
	if (callInstruction < (regionStart + 2))
		return -1;

	if (callInstruction[-2] != 0x6A)
		return -1;

	return callInstruction[-1];
}

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

void LogMessage(const char* format, ...)
{
	char buffer[512] = {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	OutputDebugStringA(buffer);
}

void LogDiagnostic(const char* format, ...)
{
	if (!framerateDiagnostics)
		return;

	char buffer[512] = {};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	OutputDebugStringA(buffer);
}

FrameTimerState& GetFrameTimerState(FrameTimerCallsite callsite)
{
	return frameTimerStates[static_cast<std::size_t>(callsite)];
}

FrameTimerCallsite GetFrameTimerCallsite(uintptr_t returnAddress)
{
	if (returnAddress == gameplayFrameTimerReturnAddress)
		return FrameTimerCallsite::Gameplay;
	if (returnAddress == frontendFrameTimerReturnAddress)
		return FrameTimerCallsite::Frontend;
	if (returnAddress == menuFrameTimerReturnAddress)
		return FrameTimerCallsite::Menu;
	return FrameTimerCallsite::Unknown;
}

bool IsStartupGuardActive()
{
	if (startupGuardMs == 0 || framerateInitTickMs == 0)
		return false;
	return (GetTickCount64() - framerateInitTickMs) < startupGuardMs;
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
	LogMessage("ToyStory2Fix: FrameTimer %s %s -> %s (%s)\n",
		GetCallsiteName(callsite), GetModeName(oldMode), GetModeName(newMode), reason);
}

int64_t QueryElapsedMicroseconds(const LARGE_INTEGER& previousTime, LARGE_INTEGER& currentTime)
{
	QueryPerformanceCounter(&currentTime);
	if (Frequency.QuadPart == 0)
		return 0;

	int64_t elapsedUs = currentTime.QuadPart - previousTime.QuadPart;
	elapsedUs *= 1000000;
	elapsedUs /= Frequency.QuadPart;
	return elapsedUs;
}

void InitializeFrameTimerModes()
{
	for (auto& state : frameTimerStates)
	{
		state.mode = FrameTimerMode::LegacyPassthrough;
		state.modeSwitchCount = 0;
		ResetFrameTimerState(state);
	}

	// Gameplay should always run in custom mode; zero-step only when safety patches are available.
	SetFrameTimerMode(FrameTimerCallsite::Gameplay,
		zeroSpeedSafetyReady ? FrameTimerMode::CustomZeroStep : FrameTimerMode::CustomSafe60,
		"initial setup");

	if (allowFrontendCustomTiming)
	{
		const FrameTimerMode frontendMode =
			(allowFrontendZeroStep && zeroSpeedSafetyReady) ? FrameTimerMode::CustomZeroStep : FrameTimerMode::CustomSafe60;
		SetFrameTimerMode(FrameTimerCallsite::Frontend, frontendMode, "initial setup");
		SetFrameTimerMode(FrameTimerCallsite::Menu, frontendMode, "initial setup");
	}
	else
	{
		LogDiagnostic("ToyStory2Fix: Frontend/Menu frame timer running in legacy mode.\n");
	}
}

void HandleAnomalyFallback(FrameTimerCallsite callsite, FrameTimerState& state, bool isDemoMode, int speedMultiplier, int frameTimeUs)
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

	if (!autoFallbackTo60)
		return;

	uint32_t zeroFrameThreshold = 120;
	uint64_t zeroStepTimeThresholdUs = 2000000ULL;

	if (callsite != FrameTimerCallsite::Gameplay && IsStartupGuardActive())
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
	{
		SetFrameTimerMode(callsite, FrameTimerMode::CustomSafe60, "anomaly detected");
	}
	else
	{
		SetFrameTimerMode(callsite, FrameTimerMode::LegacyPassthrough, "anomaly detected");
	}
}

void ApplyRefreshRate(uint32_t refreshRate)
{
	if (refreshRate < 30 || refreshRate > 1000)
		refreshRate = 60;

	targetFrameTimeUs = (1000000 + (refreshRate / 2)) / refreshRate;
	for (auto& state : frameTimerStates)
		ResetFrameTimerState(state);
}

uint32_t GetDesktopRefreshRate()
{
	DEVMODEA devMode = {};
	devMode.dmSize = sizeof(devMode);
	// This intentionally queries the primary display mode, not a window-specific monitor.
	if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &devMode) &&
		devMode.dmDisplayFrequency > 1 &&
		devMode.dmDisplayFrequency != 0xFFFFFFFFu)
	{
		return devMode.dmDisplayFrequency;
	}

	return 60;
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

	DEVMODEA devMode = {};
	devMode.dmSize = sizeof(devMode);
	if (EnumDisplaySettingsA(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode) &&
		devMode.dmDisplayFrequency > 1 &&
		devMode.dmDisplayFrequency != 0xFFFFFFFFu)
	{
		return devMode.dmDisplayFrequency;
	}

	return 0;
}

struct Variables
{
	uint32_t nWidth;
	uint32_t nHeight;
	float fAspectRatio;
	float fScaleValue;
	float f2DScaleValue;
	uint32_t* speedMultiplier;
	bool* isDemoMode;
} Variables;

int GetSafeSpeedMultiplierValue()
{
	if (Variables.speedMultiplier == nullptr)
		return 1;
	return std::max(static_cast<int>(*Variables.speedMultiplier), 1);
}

uint32_t MultiplyBySafeSpeed(uint32_t value)
{
	return static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(value)) * GetSafeSpeedMultiplierValue());
}

void DivideEaxEdxBySafeSpeed(injector::reg_pack& regs)
{
	const int64_t dividend =
		(static_cast<int64_t>(static_cast<int32_t>(regs.edx)) << 32) |
		static_cast<uint32_t>(regs.eax);
	const int32_t divisor = GetSafeSpeedMultiplierValue();
	const int32_t quotient = static_cast<int32_t>(dividend / divisor);
	const int32_t remainder = static_cast<int32_t>(dividend % divisor);

	regs.eax = static_cast<uint32_t>(quotient);
	regs.edx = static_cast<uint32_t>(remainder);
}

bool InstallZeroSpeedSafetyPatches()
{
	static bool installed = false;
	static bool ready = false;
	if (installed)
		return ready;
	installed = true;

	// The game has a few hard idiv sites on speedMultiplier.
	// These hooks keep >60Hz simulation skipping (speedMultiplier = 0) from faulting.
	bool hasMulPatch = false;
	bool hasDivPatch = false;
	bool hasMenuDivPatch = false;
	bool hasRenderDeltaPatch = false;
	bool hasRenderMotionPatch = false;

	auto pattern = hook::pattern("8B 46 68 8B 56 70 0F AF 05 ? ? ? ? 89 46 68 8B 0D ? ? ? ? 0F AF 4E 6C 89 4E 6C 0F AF 15 ? ? ? ?");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEaxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.eax = MultiplyBySafeSpeed(regs.eax);
			}
		}; injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(6), pattern.get_first(13));

		struct LoadSafeSpeedToEcxHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.ecx = static_cast<uint32_t>(GetSafeSpeedMultiplierValue());
			}
		}; injector::MakeInline<LoadSafeSpeedToEcxHook>(pattern.get_first(16), pattern.get_first(22));

		struct MulEdxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.edx = MultiplyBySafeSpeed(regs.edx);
			}
		}; injector::MakeInline<MulEdxBySafeSpeedHook>(pattern.get_first(29), pattern.get_first(36));

		hasMulPatch = true;
	}

	pattern = hook::pattern("8B 46 68 83 C4 08 99 F7 3D ? ? ? ? 89 46 68 8B 46 6C 99 F7 3D ? ? ? ? 89 46 6C 8B 46 70 99 F7 3D ? ? ? ?");
	if (!pattern.count_hint(1).empty())
	{
		struct DivBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				DivideEaxEdxBySafeSpeed(regs);
			}
		};

		injector::MakeInline<DivBySafeSpeedHook>(pattern.get_first(7), pattern.get_first(13));
		injector::MakeInline<DivBySafeSpeedHook>(pattern.get_first(20), pattern.get_first(26));
		injector::MakeInline<DivBySafeSpeedHook>(pattern.get_first(33), pattern.get_first(39));
		hasDivPatch = true;
	}

	pattern = hook::pattern("B8 1E 00 00 00 53 99 F7 3D ? ? ? ? 56 57");
	if (!pattern.count_hint(1).empty())
	{
		struct DivBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				DivideEaxEdxBySafeSpeed(regs);
			}
		}; injector::MakeInline<DivBySafeSpeedHook>(pattern.get_first(7), pattern.get_first(13));
		hasMenuDivPatch = true;
	}

	// Gameplay render path accumulates a frame delta derived from speedMultiplier.
	// Clamp that read to at least 1 so zero-step simulation doesn't stall rendering.
	pattern = hook::pattern("8B 0D ? ? ? ? B8 B7 60 0B B6 C1 E1 10 F7 E9 03 D1 C1 FA 08 8B C2 C1 E8 1F 03 D0 52 E8 ? ? ? ? 83 C4 0C");
	if (!pattern.count_hint(1).empty())
	{
		struct LoadRenderSafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.ecx = static_cast<uint32_t>(GetSafeSpeedMultiplierValue());
			}
		}; injector::MakeInline<LoadRenderSafeSpeedHook>(pattern.get_first(0), pattern.get_first(6));
		hasRenderDeltaPatch = true;
	}

	// Additional render/camera smoothing code paths that should never fully stall.
	// These are updated every frame and become visually unstable when speedMultiplier is 0.
	bool hasRenderMotionPatchA = false;
	pattern = hook::pattern("8B C1 2B C2 0F AF 05 ? ? ? ? 99 83 E2 0F 03 C2 C1 F8 04 2B C8 89 4F 04 8B 4F 08 8B 15 ? ? ? ? 8B C1 2B C2 0F AF 05 ? ? ? ? 99 83 E2 0F");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEaxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.eax = MultiplyBySafeSpeed(regs.eax);
			}
		};
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(4), pattern.get_first(11));
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(38), pattern.get_first(45));
		hasRenderMotionPatchA = true;
	}

	bool hasRenderMotionPatchB = false;
	pattern = hook::pattern("8B 46 6C 0F AF 05 ? ? ? ? 99 83 E2 0F 03 C2 C1 F8 04 66 01 46 0E");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEaxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.eax = MultiplyBySafeSpeed(regs.eax);
			}
		}; injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(3), pattern.get_first(10));
		hasRenderMotionPatchB = true;
	}

	bool hasRenderMotionPatchC = false;
	pattern = hook::pattern("C1 F8 03 0F AF 05 ? ? ? ? 99 2B C2 8B D0 66 8B 45 0E D1 FA 66 2B C2");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEaxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.eax = MultiplyBySafeSpeed(regs.eax);
			}
		}; injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(3), pattern.get_first(10));
		hasRenderMotionPatchC = true;
	}

	bool hasRenderMotionPatchD = false;
	pattern = hook::pattern("8B D1 0F AF 15 ? ? ? ? 03 C2 8B 15 ? ? ? ? 3B C2");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEdxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.edx = MultiplyBySafeSpeed(regs.edx);
			}
		}; injector::MakeInline<MulEdxBySafeSpeedHook>(pattern.get_first(2), pattern.get_first(9));
		hasRenderMotionPatchD = true;
	}

	bool hasRenderMotionPatchE = false;
	pattern = hook::pattern("33 C0 8A C1 8B 54 24 ? D1 E8 83 C0 05 83 C4 10 0F AF 05 ? ? ? ? 03 D0 83 FA 10");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEaxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.eax = MultiplyBySafeSpeed(regs.eax);
			}
		}; injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(16), pattern.get_first(23));
		hasRenderMotionPatchE = true;
	}

	bool hasRenderMotionPatchF = false;
	pattern = hook::pattern("33 C0 56 A0 ? ? ? ? 57 0F AF 05 ? ? ? ? 99 2B C2 33 FF 8B F0 D1 FE");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEaxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.eax = MultiplyBySafeSpeed(regs.eax);
			}
		}; injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(9), pattern.get_first(16));
		hasRenderMotionPatchF = true;
	}

	bool hasRenderMotionPatchG = false;
	pattern = hook::pattern("A1 ? ? ? ? 66 8B 15 ? ? ? ? C1 F8 04 0F AF 05 ? ? ? ? 8B 0D ? ? ? ? 83 C4 28");
	if (!pattern.count_hint(1).empty())
	{
		struct MulEaxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.eax = MultiplyBySafeSpeed(regs.eax);
			}
		}; injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(15), pattern.get_first(22));
		hasRenderMotionPatchG = true;
	}

	hasRenderMotionPatch = hasRenderMotionPatchA &&
		hasRenderMotionPatchB &&
		hasRenderMotionPatchC &&
		hasRenderMotionPatchD &&
		hasRenderMotionPatchE &&
		hasRenderMotionPatchF &&
		hasRenderMotionPatchG;

	if (!hasMulPatch)
		LogMessage("ToyStory2Fix: Zero-step safety missing multiply patch.\n");
	if (!hasDivPatch)
		LogMessage("ToyStory2Fix: Zero-step safety missing divide patch.\n");
	if (!hasMenuDivPatch)
		LogMessage("ToyStory2Fix: Zero-step safety missing menu divide patch.\n");
	if (!hasRenderDeltaPatch)
		LogMessage("ToyStory2Fix: Zero-step safety missing render delta patch.\n");
	if (!hasRenderMotionPatch)
		LogMessage("ToyStory2Fix: Zero-step safety missing render motion patch set.\n");

	ready = hasMulPatch && hasDivPatch && hasMenuDivPatch && hasRenderDeltaPatch && hasRenderMotionPatch;
	return ready;
}

int RunCustomFrameTimer(FrameTimerCallsite callsite, FrameTimerState& state, bool allowZeroStepSimulation)
{
	const UINT timerPeriod = tc.wPeriodMin ? tc.wPeriodMin : 1;
	const int frameTimeUs = std::max(targetFrameTimeUs, 1);
	const bool isDemoMode = *Variables.isDemoMode;
	constexpr int gameplayFrameTimeUs = 16667;
	int effectiveFrameTimeUs = frameTimeUs;
	if (isDemoMode)
	{
		// Keep demo mode pacing tied to the original 60->30 behavior.
		effectiveFrameTimeUs = 16667;
	}
	else if (!allowZeroStepSimulation && frameTimeUs < gameplayFrameTimeUs)
	{
		// If zero-step simulation is unavailable, keep simulation at safe 60 Hz pacing.
		effectiveFrameTimeUs = gameplayFrameTimeUs;
	}
	timeBeginPeriod(timerPeriod);

	if (state.previousTime.QuadPart == 0)
		QueryPerformanceCounter(&state.previousTime);
	if (state.nextFrameDeadlineQpc == 0)
		state.nextFrameDeadlineQpc = state.previousTime.QuadPart;

	LARGE_INTEGER currentTime = {};
	int64_t elapsedUs = QueryElapsedMicroseconds(state.previousTime, currentTime);
	if (elapsedUs < 0)
		elapsedUs = 0;

	if (isDemoMode)
	{
		state.simulationAccumulatorUs = 0;
		framerateFactor = (static_cast<int>(elapsedUs) / gameplayFrameTimeUs) + 1;
		// Demo mode needs 30fps maximum.
		if (framerateFactor < 2)
			framerateFactor = 2;
		framerateFactor = std::clamp(framerateFactor, 1, 3);
		*Variables.speedMultiplier = static_cast<uint32_t>(framerateFactor);
		state.consecutiveZeroFrames = 0;
		state.framesSinceNonZero = 0;
	}
	else
	{
		// Keep simulation updates anchored to the original 60 Hz timing.
		state.simulationAccumulatorUs += elapsedUs;
		const int64_t maxAccumulatorUs = static_cast<int64_t>(gameplayFrameTimeUs) * 16;
		if (state.simulationAccumulatorUs > maxAccumulatorUs)
			state.simulationAccumulatorUs = maxAccumulatorUs;

		const int desiredSteps = std::clamp(static_cast<int>(state.simulationAccumulatorUs / gameplayFrameTimeUs), 0, 3);
		if (desiredSteps > 0)
			state.simulationAccumulatorUs -= static_cast<int64_t>(desiredSteps) * gameplayFrameTimeUs;

		const int minSpeedMultiplier = allowZeroStepSimulation ? 0 : 1;
		framerateFactor = std::clamp(desiredSteps, minSpeedMultiplier, 3);

		// If we had to force a minimum of 1, consume that fixed-step from the accumulator too.
		if (framerateFactor > desiredSteps)
		{
			state.simulationAccumulatorUs -= static_cast<int64_t>(framerateFactor - desiredSteps) * gameplayFrameTimeUs;
			if (state.simulationAccumulatorUs < 0)
				state.simulationAccumulatorUs = 0;
		}

		*Variables.speedMultiplier = static_cast<uint32_t>(framerateFactor);
		HandleAnomalyFallback(callsite, state, false, framerateFactor, frameTimeUs);
	}

	const int pacingFactor = isDemoMode ? std::max(framerateFactor, 2) : 1;
	const int pacingFrameTimeUs = effectiveFrameTimeUs * pacingFactor;
	const int64_t frameDurationQpc =
		std::max<int64_t>(1, (static_cast<int64_t>(pacingFrameTimeUs) * Frequency.QuadPart + 500000) / 1000000);

	int64_t targetDeadlineQpc = state.nextFrameDeadlineQpc + frameDurationQpc;
	const int64_t maxLagQpc = frameDurationQpc * 4;
	if (currentTime.QuadPart > targetDeadlineQpc + maxLagQpc)
		targetDeadlineQpc = currentTime.QuadPart + frameDurationQpc;

	// Preserve a larger sleep guard for high-refresh rates where Sleep(1) jitter is costly.
	const bool highRefreshPacing = pacingFrameTimeUs <= 10000;
	const int64_t sleepMarginUs = highRefreshPacing
		? std::max<int64_t>(3500, pacingFrameTimeUs / 2)
		: std::max<int64_t>(2000, pacingFrameTimeUs / 5);
	const int64_t yieldMarginUs = highRefreshPacing
		? std::max<int64_t>(1200, pacingFrameTimeUs / 6)
		: std::max<int64_t>(800, pacingFrameTimeUs / 10);
	const int64_t spinMarginUs = highRefreshPacing ? 300 : 150;

	sleepTime = 0;
	for (;;)
	{
		QueryPerformanceCounter(&currentTime);
		const int64_t remainingQpc = targetDeadlineQpc - currentTime.QuadPart;
		const int64_t remainingUs = (remainingQpc * 1000000) / Frequency.QuadPart;
		if (remainingUs <= 0)
			break;

		if (remainingUs > sleepMarginUs)
		{
			// Keep a guard window before the frame deadline to avoid oversleep jitter.
			const int64_t sleepCandidateUs = remainingUs - sleepMarginUs;
			const int candidateMs = static_cast<int>(sleepCandidateUs / 1000);
			if (candidateMs > 0)
			{
				sleepTime = candidateMs;
				Sleep(sleepTime);
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
	// Match the original function contract: return millisecond clock value.
	return static_cast<int>(timeGetTime());
}

int __cdecl sub_490860(int a1)
{
	auto _sub_490860 = (int(__cdecl*)(int))sub_490860_addr;
	if (Variables.speedMultiplier == nullptr || Variables.isDemoMode == nullptr)
		return _sub_490860 ? _sub_490860(a1) : 0;

	if (refreshRateProbePending)
	{
		uint32_t processRefreshRate = GetProcessWindowRefreshRate();
		if (processRefreshRate > 0)
		{
			ApplyRefreshRate(processRefreshRate);
			refreshRateProbePending = false;
		}
	}

	const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(_ReturnAddress());
	const FrameTimerCallsite callsite = GetFrameTimerCallsite(returnAddress);
	if (callsite == FrameTimerCallsite::Unknown)
		return _sub_490860 ? _sub_490860(a1) : 0;

	auto& state = GetFrameTimerState(callsite);

	// During early startup, keep all paths on the original game timer.
	if (IsStartupGuardActive())
		return _sub_490860 ? _sub_490860(a1) : 0;

	if (state.mode == FrameTimerMode::LegacyPassthrough)
		return _sub_490860 ? _sub_490860(a1) : 0;

	const bool allowZeroStepSimulation = (state.mode == FrameTimerMode::CustomZeroStep) && zeroSpeedSafetyReady;
	LogDiagnostic("ToyStory2Fix: FrameTimer %s mode=%s a1=%d\n",
		GetCallsiteName(callsite), GetModeName(state.mode), a1);
	return RunCustomFrameTimer(callsite, state, allowZeroStepSimulation);
}

int sub_49D910() {
	auto _sub_49D910 = (int(*)())sub_49D910_addr;

	/* FIX WIDESCREEN */
	// this code can't be in Init() because width/height are not set at first
	static bool bResolutionPointerLookupAttempted = false;
	static uint32_t* pResolution = nullptr;
	static bool bWidescreen3DHookAttempted = false;

	/* Set width and height */
	if (!bResolutionPointerLookupAttempted)
	{
		auto pattern = hook::pattern("8B 15 ? ? ? ? 89 4C 24 08 89 44 24 0C"); //4B5672
		if (!pattern.count_hint(1).empty())
			pResolution = *reinterpret_cast<uint32_t**>(pattern.get_first(2));
		bResolutionPointerLookupAttempted = true;
	}

	if (pResolution != nullptr)
	{
		Variables.nWidth = pResolution[0];
		Variables.nHeight = pResolution[1];
	}

	if (Variables.nWidth == 0 || Variables.nHeight == 0)
		return _sub_49D910();

	Variables.fAspectRatio = float(Variables.nWidth) / float(Variables.nHeight);
	Variables.fScaleValue = 1.0f / Variables.fAspectRatio;
	Variables.f2DScaleValue = (4.0f / 3.0f) / Variables.fAspectRatio;

	/* Fix 3D stretch */
	if (!bWidescreen3DHookAttempted)
	{
		bWidescreen3DHookAttempted = true;
		auto pattern = hook::pattern("C7 40 44 00 00 40 3F"); //4CE80F
		if (!pattern.count_hint(1).empty())
		{
			struct Widescreen3DHook
			{
				void operator()(injector::reg_pack& regs)
				{
					float* ptrScaleValue = reinterpret_cast<float*>(regs.eax + 0x44);
					*ptrScaleValue = Variables.fScaleValue;
				}
			}; injector::MakeInline<Widescreen3DHook>(pattern.get_first(0), pattern.get_first(6));
		}
	}

	return _sub_49D910();
}

DWORD WINAPI Init(LPVOID bDelay)
{
	/* INITIALISE */
	auto pattern = hook::pattern("03 D1 2B D7 85 D2 7E 09 52 E8 ? ? ? ?"); //4909B5
	constexpr ULONGLONG kInitTimeoutMs = 10000;
	constexpr DWORD kInitRetrySleepMs = 10;

	if (pattern.count_hint(1).empty() && !bDelay)
	{
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)&Init, (LPVOID)true, 0, NULL);
		return 0;
	}

	if (bDelay)
	{
		const ULONGLONG startMs = GetTickCount64();
		while (pattern.clear().count_hint(1).empty())
		{
			if (GetTickCount64() - startMs >= kInitTimeoutMs)
			{
				OutputDebugStringA("ToyStory2Fix: Init timeout waiting for target pattern.");
				return 0;
			}
			Sleep(kInitRetrySleepMs);
		}
	}

	CIniReader iniReader("ToyStory2Fix.ini");
	constexpr const char* INI_KEY = "ToyStory2Fix";

	if (iniReader.ReadBoolean(INI_KEY, "FixFramerate", true)) {
		if (timeGetDevCaps(&tc, sizeof(tc)) != TIMERR_NOERROR || tc.wPeriodMin == 0)
			tc.wPeriodMin = 1;
		if (!QueryPerformanceFrequency(&Frequency) || Frequency.QuadPart == 0)
			Frequency.QuadPart = 1;

		framerateDiagnostics = iniReader.ReadBoolean(INI_KEY, "FramerateDiagnostics", false);
		autoFallbackTo60 = iniReader.ReadBoolean(INI_KEY, "AutoFallbackTo60", true);
		allowFrontendCustomTiming = iniReader.ReadBoolean(INI_KEY, "AllowFrontendCustomTiming", false);
		allowFrontendZeroStep = iniReader.ReadBoolean(INI_KEY, "AllowFrontendZeroStep", false);
		startupGuardMs = static_cast<uint32_t>(std::max(0, iniReader.ReadInteger(INI_KEY, "StartupGuardMs", 5000)));
		if (!allowFrontendCustomTiming)
			allowFrontendZeroStep = false;
		framerateInitTickMs = GetTickCount64();

		gameplayFrameTimerReturnAddress = 0;
		frontendFrameTimerReturnAddress = 0;
		menuFrameTimerReturnAddress = 0;

		targetRefreshRateOverride = std::max(0, iniReader.ReadInteger(INI_KEY, "TargetRefreshRate", 0));
		refreshRateProbePending = false;
		if (iniReader.ReadBoolean(INI_KEY, "NativeRefreshRate", false))
		{
			uint32_t refreshRate = targetRefreshRateOverride > 0 ? targetRefreshRateOverride : GetDesktopRefreshRate();
			ApplyRefreshRate(refreshRate);
			// Retry once the game window exists to support multi-monitor setups where primary display differs.
			refreshRateProbePending = (targetRefreshRateOverride == 0);
		}
		else
		{
			ApplyRefreshRate(60);
		}

		pattern = hook::pattern("8B 0D ? ? ? ? 2B F1 3B"); //4011DF
		if (!pattern.count_hint(1).empty())
			Variables.speedMultiplier = *reinterpret_cast<uint32_t**>(pattern.get_first(2));

		pattern = hook::pattern("39 3D ? ? ? ? 75 27"); //403C3A
		if (!pattern.count_hint(1).empty())
			Variables.isDemoMode = *reinterpret_cast<bool**>(pattern.get_first(2));

		zeroSpeedSafetyReady = InstallZeroSpeedSafetyPatches();
		if (!zeroSpeedSafetyReady)
			OutputDebugStringA("ToyStory2Fix: High-refresh simulation safety patches unavailable. Falling back to legacy framerate behavior.");
		if (allowFrontendZeroStep && !zeroSpeedSafetyReady)
		{
			allowFrontendZeroStep = false;
			LogMessage("ToyStory2Fix: AllowFrontendZeroStep disabled because safety patches were unavailable.\n");
		}

		pattern = hook::pattern("C7 05 ? ? ? ? 00 00 00 00 E8 ? ? ? ? E8 ? ? ? ? 33"); //49BBD8
		if (!pattern.count_hint(1).empty())
		{
			auto* callInstruction = pattern.get_first<uint8_t>(10);
			menuFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
			sub_490860_addr = ResolveRelativeCall(callInstruction);
			injector::MakeCALL(callInstruction, sub_490860);
		}

		if (sub_490860_addr != 0)
		{
			auto timerCallsites = FindDirectCallsToTarget(sub_490860_addr);
			for (auto* callInstruction : timerCallsites)
			{
				const uintptr_t returnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
				if (returnAddress == menuFrameTimerReturnAddress)
					continue;

				const int pushedArg = GetImmediatePushArgBeforeCall(callInstruction);
				if (pushedArg == 1 && gameplayFrameTimerReturnAddress == 0)
					gameplayFrameTimerReturnAddress = returnAddress;
				else if (pushedArg == 0 && frontendFrameTimerReturnAddress == 0)
					frontendFrameTimerReturnAddress = returnAddress;

				injector::MakeCALL(callInstruction, sub_490860);
			}

			if (gameplayFrameTimerReturnAddress == 0)
			{
				pattern = hook::pattern("83 C4 08 6A 01 E8 ? ? ? ?");
				if (!pattern.count_hint(1).empty())
				{
					auto* callInstruction = pattern.get_first<uint8_t>(5);
					gameplayFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
					injector::MakeCALL(callInstruction, sub_490860);
				}
			}

			if (frontendFrameTimerReturnAddress == 0)
			{
				pattern = hook::pattern("E8 ? ? ? ? 6A 00 E8 ? ? ? ? 6A 01 E8 ? ? ? ? 83 C4");
				if (!pattern.count_hint(1).empty())
				{
					auto* callInstruction = pattern.get_first<uint8_t>(7);
					frontendFrameTimerReturnAddress = reinterpret_cast<uintptr_t>(callInstruction + 5);
					injector::MakeCALL(callInstruction, sub_490860);
				}
			}

			LogMessage("ToyStory2Fix: FrameTimer callsites gameplay=%p frontend=%p menu=%p\n",
				reinterpret_cast<void*>(gameplayFrameTimerReturnAddress),
				reinterpret_cast<void*>(frontendFrameTimerReturnAddress),
				reinterpret_cast<void*>(menuFrameTimerReturnAddress));

			InitializeFrameTimerModes();
		}
		else
		{
			LogMessage("ToyStory2Fix: Could not resolve frame-timer target. Legacy game timing will be used.\n");
		}
	}


	/* Allow 32-bit modes regardless of registry settings - thanks hdc0 */
	if (iniReader.ReadBoolean(INI_KEY, "Allow32Bit", true)) {
		pattern = hook::pattern("74 0B 5E 5D B8 01 00 00 00"); //4ACA44
		if (!pattern.count_hint(1).empty())
			injector::WriteMemory<uint8_t>(pattern.get_first(0), '\xEB', true);
	}

	/* Fix "Unable to enumerate a suitable device - thanks hdc0 */
	if (iniReader.ReadBoolean(INI_KEY, "IgnoreVRAM", true)) {
		pattern = hook::pattern("74 44 8B 8A 50 01 00 00 8B 91 64 03 00 00"); //4ACAC2
		if (!pattern.count_hint(1).empty())
			injector::WriteMemory<uint8_t>(pattern.get_first(0), '\xEB', true);
	}

	/* Allow copyright/ESRB screen to be skipped immediately */
	if (iniReader.ReadBoolean(INI_KEY, "SkipSplash", true)) {
		pattern = hook::pattern("66 8B 3D ? ? ? ? 83 C4 1C"); //438586
		if (!pattern.count_hint(1).empty())
		{
			struct CopyrightHook
			{
				void operator()(injector::reg_pack& regs)
				{
					regs.edi = (regs.edi & 0xFFFF0000u) | 1u;
				}

			}; injector::MakeInline<CopyrightHook>(pattern.get_first(0), pattern.get_first(7));
		}
	}

	/* Increase Render Distance to Max */
	if (iniReader.ReadBoolean(INI_KEY, "IncreaseRenderDistance", true)) {
		pattern = hook::pattern("D9 44 24 04 D8 4C 24 04 D9 1D"); //4BC410
		if (!pattern.count_hint(1).empty())
		{
			auto* pRenderDistance = *reinterpret_cast<float**>(pattern.get_first(10));
			if (pRenderDistance != nullptr)
				*pRenderDistance = INFINITY;
			injector::MakeNOP(pattern.get_first(8), 6);
		}
	}

	/* Fix widescreen once game loop begins */
	if (iniReader.ReadBoolean(INI_KEY, "Widescreen", true)) {
		pattern = hook::pattern("8D 44 24 10 50 57 E8 ? ? ? ? 83"); //4317EC
		if (!pattern.count_hint(1).empty())
		{
			auto* callInstruction = pattern.get_first<uint8_t>(6);
			sub_49D910_addr = ResolveRelativeCall(callInstruction);
			injector::MakeCALL(callInstruction, sub_49D910);
		}
	}

	return 0;
}


BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*lpReserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        Init(NULL);
    }
    return TRUE;
}
