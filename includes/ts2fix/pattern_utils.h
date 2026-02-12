#pragma once

#include <cstdint>
#include <vector>

namespace ts2fix
{
uintptr_t ResolveRelativeCall(uint8_t* callInstruction);
std::vector<uint8_t*> FindDirectCallsToTarget(uintptr_t targetAddress);
int GetImmediatePushArgBeforeCall(uint8_t* callInstruction);
} // namespace ts2fix
