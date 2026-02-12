#include "stdafx.h"
#include <MMSystem.h>

uintptr_t sub_490860_addr;
uintptr_t sub_49D910_addr;
TIMECAPS tc;
LARGE_INTEGER Frequency;
LARGE_INTEGER PreviousTime, CurrentTime, ElapsedMicroseconds;
int sleepTime;
int framerateFactor;
int targetFrameTimeUs = 16667;
int sleepFrameBudgetUs = 16949;
bool refreshRateProbePending = false;
uint32_t targetRefreshRateOverride = 0;

uintptr_t ResolveRelativeCall(uint8_t* callInstruction)
{
	int32_t relativeTarget = *reinterpret_cast<int32_t*>(callInstruction + 1);
	return reinterpret_cast<uintptr_t>(callInstruction + 5 + relativeTarget);
}

void ApplyRefreshRate(uint32_t refreshRate)
{
	if (refreshRate < 30 || refreshRate > 1000)
		refreshRate = 60;

	targetFrameTimeUs = (1000000 + (refreshRate / 2)) / refreshRate;
	sleepFrameBudgetUs = targetFrameTimeUs + std::max(1, targetFrameTimeUs / 60);
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

void UpdateElapsedMicroseconds() {
	if (Frequency.QuadPart == 0)
	{
		ElapsedMicroseconds.QuadPart = 0;
		return;
	}

	QueryPerformanceCounter(&CurrentTime);
	ElapsedMicroseconds.QuadPart = CurrentTime.QuadPart - PreviousTime.QuadPart;
	ElapsedMicroseconds.QuadPart *= 1000000;
	ElapsedMicroseconds.QuadPart /= Frequency.QuadPart;
}

int __cdecl sub_490860(int a1) {
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

	const UINT timerPeriod = tc.wPeriodMin ? tc.wPeriodMin : 1;
	const int frameTimeUs = std::max(targetFrameTimeUs, 1);
	const int frameBudgetUs = std::max(sleepFrameBudgetUs, frameTimeUs);
	const bool isDemoMode = *Variables.isDemoMode;
	constexpr int gameplayFrameTimeUs = 16667;
	// Keep demo mode pacing tied to the original 60->30 behavior.
	const int effectiveFrameTimeUs = isDemoMode ? 16667 : frameTimeUs;
	const int effectiveFrameBudgetUs = isDemoMode ? 16949 : frameBudgetUs;
	timeBeginPeriod(timerPeriod);

	if (PreviousTime.QuadPart == 0)
		QueryPerformanceCounter(&PreviousTime); // initialise

	UpdateElapsedMicroseconds();

	// Keep simulation multiplier anchored to the original 60 Hz update timing.
	// Native refresh only controls frame pacing, not gameplay speed scaling.
	framerateFactor = ((int)ElapsedMicroseconds.QuadPart / gameplayFrameTimeUs) + 1;
	// Demo mode needs 30fps maximum
	if (isDemoMode && framerateFactor < 2)
		framerateFactor = 2;
		
	*Variables.speedMultiplier = std::clamp(framerateFactor, 1, 3);

	sleepTime = 0;
	// Loop until next frame due
	do {
		sleepTime = (effectiveFrameBudgetUs * framerateFactor - (uint32_t)ElapsedMicroseconds.QuadPart) / 1000; // sleep slightly past target frame to limit frame drops
		sleepTime = ((sleepTime / timerPeriod) * timerPeriod) - timerPeriod; // truncate to multiple of period
		if (sleepTime > 0)
			Sleep(sleepTime); // sleep to avoid wasted CPU
		UpdateElapsedMicroseconds();
	} while (ElapsedMicroseconds.QuadPart < effectiveFrameTimeUs * framerateFactor);

	QueryPerformanceCounter(&PreviousTime);
	timeEndPeriod(timerPeriod);
	return (int)(PreviousTime.QuadPart / 1000);
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

		pattern = hook::pattern("C7 05 ? ? ? ? 00 00 00 00 E8 ? ? ? ? E8 ? ? ? ? 33"); //49BBD8
		if (!pattern.count_hint(1).empty())
		{
			auto* callInstruction = pattern.get_first<uint8_t>(10);
			sub_490860_addr = ResolveRelativeCall(callInstruction);
			injector::MakeCALL(callInstruction, sub_490860);
		}

		if (sub_490860_addr != 0)
		{
			pattern = hook::pattern("83 C4 08 6A 01 E8 ? ? ? ?"); //441906
			if (!pattern.count_hint(1).empty())
				injector::MakeCALL(pattern.get_first(5), sub_490860);

			pattern = hook::pattern("6A 00 E8 ? ? ? ? 6A 01 E8 ? ? ? ? 83"); //4419F4
			if (!pattern.count_hint(1).empty())
				injector::MakeCALL(pattern.get_first(2), sub_490860);
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
