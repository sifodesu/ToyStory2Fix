#include "stdafx.h"
#include "ts2fix/config.h"
#include "ts2fix/logging.h"

#include <algorithm>

namespace
{
constexpr const char* kLegacySection = "ToyStory2Fix";

bool HasKey(CIniReader& iniReader, const char* section, const char* key)
{
	const auto sectionIt = iniReader.data.find(section);
	if (sectionIt == iniReader.data.end())
		return false;
	return sectionIt->second.find(key) != sectionIt->second.end();
}

void LogLegacyAliasUse(const char* legacyKey, const char* section, const char* key)
{
	ts2fix::Log("Config", "Legacy key [%s]/%s mapped to [%s]/%s.\n", kLegacySection, legacyKey, section, key);
}

bool ReadBooleanWithAlias(CIniReader& iniReader, const char* section, const char* key, bool defaultValue, const char* legacyKey)
{
	if (HasKey(iniReader, section, key))
		return iniReader.ReadBoolean(section, key, defaultValue);

	if (legacyKey != nullptr && HasKey(iniReader, kLegacySection, legacyKey))
	{
		LogLegacyAliasUse(legacyKey, section, key);
		return iniReader.ReadBoolean(kLegacySection, legacyKey, defaultValue);
	}

	return defaultValue;
}

int ReadIntegerWithAlias(CIniReader& iniReader, const char* section, const char* key, int defaultValue, const char* legacyKey)
{
	if (HasKey(iniReader, section, key))
		return iniReader.ReadInteger(section, key, defaultValue);

	if (legacyKey != nullptr && HasKey(iniReader, kLegacySection, legacyKey))
	{
		LogLegacyAliasUse(legacyKey, section, key);
		return iniReader.ReadInteger(kLegacySection, legacyKey, defaultValue);
	}

	return defaultValue;
}

float ReadFloatWithAlias(CIniReader& iniReader, const char* section, const char* key, float defaultValue, const char* legacyKey)
{
	if (HasKey(iniReader, section, key))
		return iniReader.ReadFloat(section, key, defaultValue);

	if (legacyKey != nullptr && HasKey(iniReader, kLegacySection, legacyKey))
	{
		LogLegacyAliasUse(legacyKey, section, key);
		return iniReader.ReadFloat(kLegacySection, legacyKey, defaultValue);
	}

	return defaultValue;
}
} // namespace

namespace ts2fix
{
Config LoadConfig(CIniReader& iniReader)
{
	Config config = {};

	config.framerate.enabled = ReadBooleanWithAlias(iniReader, "Framerate", "enabled", true, "FixFramerate");
	config.framerate.diagnostics = ReadBooleanWithAlias(iniReader, "Framerate", "diagnostics", false, "FramerateDiagnostics");
	config.framerate.nativeRefreshRate = ReadBooleanWithAlias(iniReader, "Framerate", "native_refresh", false, "NativeRefreshRate");
	config.framerate.targetRefreshRate = std::max(0, ReadIntegerWithAlias(iniReader, "Framerate", "target_refresh_rate", 0, "TargetRefreshRate"));
	config.framerate.autoFallbackTo60 = ReadBooleanWithAlias(iniReader, "Framerate", "auto_fallback_60", true, "AutoFallbackTo60");
	config.framerate.startupGuardMs = static_cast<uint32_t>(
		std::max(0, ReadIntegerWithAlias(iniReader, "Framerate", "startup_guard_ms", 5000, "StartupGuardMs")));
	config.framerate.frontendCustomTiming = ReadBooleanWithAlias(
		iniReader, "Framerate", "frontend_custom_timing", false, "AllowFrontendCustomTiming");
	config.framerate.frontendZeroStep = ReadBooleanWithAlias(
		iniReader, "Framerate", "frontend_zero_step", false, "AllowFrontendZeroStep");

	config.rendering.widescreen = ReadBooleanWithAlias(iniReader, "Rendering", "widescreen", true, "Widescreen");
	config.rendering.increaseRenderDistance = ReadBooleanWithAlias(
		iniReader, "Rendering", "increase_render_distance", true, "IncreaseRenderDistance");
	config.rendering.renderDistanceScale = std::max(
		1.0f, ReadFloatWithAlias(iniReader, "Rendering", "render_distance_scale", 1.5f, "RenderDistanceScale"));
	config.rendering.renderDistanceMax = std::max(
		0.0f, ReadFloatWithAlias(iniReader, "Rendering", "render_distance_max", 18000.0f, "RenderDistanceMax"));

	config.compatibility.allow32Bit = ReadBooleanWithAlias(iniReader, "Compatibility", "allow_32bit", true, "Allow32Bit");
	config.compatibility.ignoreVRAM = ReadBooleanWithAlias(iniReader, "Compatibility", "ignore_vram", true, "IgnoreVRAM");
	config.compatibility.skipSplash = ReadBooleanWithAlias(iniReader, "Compatibility", "skip_splash", true, "SkipSplash");

	if (!config.framerate.frontendCustomTiming && config.framerate.frontendZeroStep)
	{
		config.framerate.frontendZeroStep = false;
		Log("Config", "Disabled frontend_zero_step because frontend_custom_timing is off.\n");
	}

	return config;
}
} // namespace ts2fix
