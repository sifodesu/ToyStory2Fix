#include "stdafx.h"
#include "ts2fix/pattern_utils.h"

namespace ts2fix
{
uintptr_t ResolveRelativeCall(uint8_t* callInstruction)
{
	const int32_t relativeTarget = *reinterpret_cast<int32_t*>(callInstruction + 1);
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

			const int32_t relativeTarget = *reinterpret_cast<int32_t*>(cursor + 1);
			const uintptr_t destination = reinterpret_cast<uintptr_t>(cursor + 5 + relativeTarget);
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

	MEMORY_BASIC_INFORMATION memoryInfo = {};
	if (VirtualQuery(callInstruction, &memoryInfo, sizeof(memoryInfo)) == 0)
		return -1;

	auto* regionStart = reinterpret_cast<uint8_t*>(memoryInfo.BaseAddress);
	if (callInstruction < (regionStart + 2))
		return -1;

	if (callInstruction[-2] != 0x6A)
		return -1;

	return callInstruction[-1];
}
} // namespace ts2fix
