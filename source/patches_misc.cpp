#include "stdafx.h"
#include "ts2fix/patches_misc.h"
#include "ts2fix/logging.h"

#include <algorithm>

namespace ts2fix
{
void ApplyMiscPatches(const Config& config)
{
	if (config.compatibility.allow32Bit)
	{
		auto pattern = hook::pattern("74 0B 5E 5D B8 01 00 00 00");
		if (!pattern.count_hint(1).empty())
			injector::WriteMemory<uint8_t>(pattern.get_first(0), '\xEB', true);
		else
			Log("Compat", "Allow32Bit pattern not found; patch skipped.\n");
	}

	if (config.compatibility.ignoreVRAM)
	{
		auto pattern = hook::pattern("74 44 8B 8A 50 01 00 00 8B 91 64 03 00 00");
		if (!pattern.count_hint(1).empty())
			injector::WriteMemory<uint8_t>(pattern.get_first(0), '\xEB', true);
		else
			Log("Compat", "IgnoreVRAM pattern not found; patch skipped.\n");
	}

	if (config.compatibility.skipSplash)
	{
		auto pattern = hook::pattern("66 8B 3D ? ? ? ? 83 C4 1C");
		if (!pattern.count_hint(1).empty())
		{
			struct CopyrightHook
			{
				void operator()(injector::reg_pack& regs)
				{
					regs.edi = (regs.edi & 0xFFFF0000u) | 1u;
				}
			};
			injector::MakeInline<CopyrightHook>(pattern.get_first(0), pattern.get_first(7));
		}
		else
		{
			Log("Compat", "SkipSplash pattern not found; patch skipped.\n");
		}
	}

	if (config.rendering.increaseRenderDistance)
	{
		const float renderDistanceScale = std::max(1.0f, config.rendering.renderDistanceScale);
		const float renderDistanceMax = std::max(0.0f, config.rendering.renderDistanceMax);

		auto pattern = hook::pattern("8B 86 ? ? ? ? 8B 8E ? ? ? ? 50 51 E8 ? ? ? ? 8B 96 ? ? ? ? 8B 86 ? ? ? ? 8B 8E ? ? ? ? 83 C4 08");
		if (!pattern.count_hint(1).empty())
		{
			auto* renderDistanceTable = reinterpret_cast<float*>(*reinterpret_cast<uint32_t*>(pattern.get_first(8)));
			if (renderDistanceTable != nullptr)
			{
				constexpr std::size_t kRenderDistanceEntryStride = 6;
				constexpr std::size_t kRenderDistanceEntryCount = 3;

				bool tableLooksValid = true;
				for (std::size_t i = 0; i < kRenderDistanceEntryCount; ++i)
				{
					const float baseDistance = renderDistanceTable[i * kRenderDistanceEntryStride];
					if (!(baseDistance >= 100.0f && baseDistance <= 100000.0f))
					{
						tableLooksValid = false;
						break;
					}
				}

				if (!tableLooksValid)
				{
					Log("RenderDistance", "Table sanity check failed; patch skipped.\n");
				}
				else
				{
					for (std::size_t i = 0; i < kRenderDistanceEntryCount; ++i)
					{
						float* distance = renderDistanceTable + (i * kRenderDistanceEntryStride);
						const float baseDistance = std::max(*distance, 1.0f);
						float boostedDistance = baseDistance * renderDistanceScale;
						if (renderDistanceMax > 0.0f)
							boostedDistance = std::min(boostedDistance, renderDistanceMax);
						*distance = boostedDistance;
					}
				}
			}
			else
			{
				Log("RenderDistance", "Render-distance table pointer was null; patch skipped.\n");
			}
		}
		else
		{
			Log("RenderDistance", "Pattern not found; patch skipped.\n");
		}
	}
}
} // namespace ts2fix
