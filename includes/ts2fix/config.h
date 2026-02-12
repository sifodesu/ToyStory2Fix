#pragma once

#include "stdafx.h"

namespace ts2fix
{
struct FramerateConfig
{
	bool enabled = true;
	bool diagnostics = false;
	bool nativeRefreshRate = false;
	int targetRefreshRate = 0;
	bool autoFallbackTo60 = true;
	uint32_t startupGuardMs = 5000;
	bool frontendCustomTiming = false;
	bool frontendZeroStep = false;
};

struct RenderingConfig
{
	bool widescreen = true;
	bool increaseRenderDistance = true;
	float renderDistanceScale = 1.5f;
	float renderDistanceMax = 18000.0f;
};

struct CompatibilityConfig
{
	bool allow32Bit = true;
	bool ignoreVRAM = true;
	bool skipSplash = true;
};

struct Config
{
	FramerateConfig framerate = {};
	RenderingConfig rendering = {};
	CompatibilityConfig compatibility = {};
};

Config LoadConfig(CIniReader& iniReader);
} // namespace ts2fix
