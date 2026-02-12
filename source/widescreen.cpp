#include "stdafx.h"
#include "ts2fix/widescreen.h"
#include "ts2fix/logging.h"
#include "ts2fix/pattern_utils.h"
#include "ts2fix/runtime.h"

namespace ts2fix
{
int WidescreenHook()
{
	auto& runtime = GetRuntimeContext();
	auto original = reinterpret_cast<int(*)()>(runtime.widescreenTargetAddress);

	// Width and height are not initialized until the game loop starts.
	static bool resolutionPointerLookupAttempted = false;
	static uint32_t* resolution = nullptr;
	static bool widescreen3DHookAttempted = false;

	if (!resolutionPointerLookupAttempted)
	{
		auto pattern = hook::pattern("8B 15 ? ? ? ? 89 4C 24 08 89 44 24 0C");
		if (!pattern.count_hint(1).empty())
			resolution = *reinterpret_cast<uint32_t**>(pattern.get_first(2));
		resolutionPointerLookupAttempted = true;
	}

	if (resolution != nullptr)
	{
		runtime.variables.nWidth = resolution[0];
		runtime.variables.nHeight = resolution[1];
	}

	if (runtime.variables.nWidth == 0 || runtime.variables.nHeight == 0)
		return original ? original() : 0;

	runtime.variables.fAspectRatio = static_cast<float>(runtime.variables.nWidth) / static_cast<float>(runtime.variables.nHeight);
	runtime.variables.fScaleValue = 1.0f / runtime.variables.fAspectRatio;
	runtime.variables.f2DScaleValue = (4.0f / 3.0f) / runtime.variables.fAspectRatio;

	if (!widescreen3DHookAttempted)
	{
		widescreen3DHookAttempted = true;
		auto pattern = hook::pattern("C7 40 44 00 00 40 3F");
		if (!pattern.count_hint(1).empty())
		{
			struct Widescreen3DHook
			{
				void operator()(injector::reg_pack& regs)
				{
					float* scaleValue = reinterpret_cast<float*>(regs.eax + 0x44);
					*scaleValue = ts2fix::GetRuntimeContext().variables.fScaleValue;
				}
			};
			injector::MakeInline<Widescreen3DHook>(pattern.get_first(0), pattern.get_first(6));
		}
	}

	return original ? original() : 0;
}

bool InstallWidescreenHook()
{
	auto pattern = hook::pattern("8D 44 24 10 50 57 E8 ? ? ? ? 83");
	if (pattern.count_hint(1).empty())
	{
		Log("Widescreen", "Pattern not found; widescreen hook skipped.\n");
		return false;
	}

	auto* callInstruction = pattern.get_first<uint8_t>(6);
	auto& runtime = GetRuntimeContext();
	runtime.widescreenTargetAddress = ResolveRelativeCall(callInstruction);
	injector::MakeCALL(callInstruction, WidescreenHook);
	return true;
}
} // namespace ts2fix
