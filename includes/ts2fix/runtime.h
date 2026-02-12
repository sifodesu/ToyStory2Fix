#pragma once

#include "stdafx.h"
#include <MMSystem.h>
#include <cstdint>

namespace ts2fix
{
struct Variables
{
	uint32_t nWidth = 0;
	uint32_t nHeight = 0;
	float fAspectRatio = 0.0f;
	float fScaleValue = 1.0f;
	float f2DScaleValue = 1.0f;
	uint32_t* speedMultiplier = nullptr;
	bool* isDemoMode = nullptr;
};

struct RuntimeContext
{
	uintptr_t frameTimerTargetAddress = 0;
	uintptr_t widescreenTargetAddress = 0;

	uintptr_t gameplayFrameTimerReturnAddress = 0;
	uintptr_t frontendFrameTimerReturnAddress = 0;
	uintptr_t menuFrameTimerReturnAddress = 0;

	TIMECAPS timerCaps = {};
	LARGE_INTEGER performanceFrequency = {};

	int sleepTime = 0;
	int framerateFactor = 0;
	int targetFrameTimeUs = 16667;

	bool refreshRateProbePending = false;
	bool zeroSpeedSafetyReady = false;
	uint32_t targetRefreshRateOverride = 0;
	ULONGLONG framerateInitTickMs = 0;

	Variables variables = {};
};

RuntimeContext& GetRuntimeContext();
} // namespace ts2fix
