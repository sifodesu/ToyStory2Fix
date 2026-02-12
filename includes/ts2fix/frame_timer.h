#pragma once

#include "ts2fix/config.h"

#include <cstdint>

namespace ts2fix
{
void ConfigureFrameTimer(const FramerateConfig& config);
void SetFrameTimerCallsiteAddresses(uintptr_t gameplayReturnAddress, uintptr_t frontendReturnAddress, uintptr_t menuReturnAddress);
void InitializeFrameTimerModes();

void ApplyRefreshRate(uint32_t refreshRate);
uint32_t GetDesktopRefreshRate();
uint32_t GetProcessWindowRefreshRate();
bool IsStartupGuardActive();

int __cdecl FrameTimerHook(int a1);
} // namespace ts2fix
