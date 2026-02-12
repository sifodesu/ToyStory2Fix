#include "stdafx.h"
#include "ts2fix/zero_speed_safety.h"
#include "ts2fix/logging.h"
#include "ts2fix/runtime.h"

#include <algorithm>
#include <cstdint>

namespace
{
int GetSafeSpeedMultiplierValue()
{
	const auto& runtime = ts2fix::GetRuntimeContext();
	if (runtime.variables.speedMultiplier == nullptr)
		return 1;
	return std::max(static_cast<int>(*runtime.variables.speedMultiplier), 1);
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
} // namespace

namespace ts2fix
{
bool InstallZeroSpeedSafetyPatches()
{
	static bool installed = false;
	static bool ready = false;
	if (installed)
		return ready;
	installed = true;

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
		};
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(6), pattern.get_first(13));

		struct LoadSafeSpeedToEcxHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.ecx = static_cast<uint32_t>(GetSafeSpeedMultiplierValue());
			}
		};
		injector::MakeInline<LoadSafeSpeedToEcxHook>(pattern.get_first(16), pattern.get_first(22));

		struct MulEdxBySafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.edx = MultiplyBySafeSpeed(regs.edx);
			}
		};
		injector::MakeInline<MulEdxBySafeSpeedHook>(pattern.get_first(29), pattern.get_first(36));

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
		};
		injector::MakeInline<DivBySafeSpeedHook>(pattern.get_first(7), pattern.get_first(13));
		hasMenuDivPatch = true;
	}

	pattern = hook::pattern("8B 0D ? ? ? ? B8 B7 60 0B B6 C1 E1 10 F7 E9 03 D1 C1 FA 08 8B C2 C1 E8 1F 03 D0 52 E8 ? ? ? ? 83 C4 0C");
	if (!pattern.count_hint(1).empty())
	{
		struct LoadRenderSafeSpeedHook
		{
			void operator()(injector::reg_pack& regs)
			{
				regs.ecx = static_cast<uint32_t>(GetSafeSpeedMultiplierValue());
			}
		};
		injector::MakeInline<LoadRenderSafeSpeedHook>(pattern.get_first(0), pattern.get_first(6));
		hasRenderDeltaPatch = true;
	}

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
		};
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(3), pattern.get_first(10));
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
		};
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(3), pattern.get_first(10));
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
		};
		injector::MakeInline<MulEdxBySafeSpeedHook>(pattern.get_first(2), pattern.get_first(9));
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
		};
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(16), pattern.get_first(23));
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
		};
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(9), pattern.get_first(16));
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
		};
		injector::MakeInline<MulEaxBySafeSpeedHook>(pattern.get_first(15), pattern.get_first(22));
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
		Log("Safety", "Zero-step safety missing multiply patch.\n");
	if (!hasDivPatch)
		Log("Safety", "Zero-step safety missing divide patch.\n");
	if (!hasMenuDivPatch)
		Log("Safety", "Zero-step safety missing menu divide patch.\n");
	if (!hasRenderDeltaPatch)
		Log("Safety", "Zero-step safety missing render delta patch.\n");
	if (!hasRenderMotionPatch)
		Log("Safety", "Zero-step safety missing render motion patch set.\n");

	ready = hasMulPatch && hasDivPatch && hasMenuDivPatch && hasRenderDeltaPatch && hasRenderMotionPatch;
	return ready;
}
} // namespace ts2fix
