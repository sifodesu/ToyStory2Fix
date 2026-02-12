#include "stdafx.h"
#include "ts2fix/logging.h"

#include <cstdarg>
#include <cstdio>

namespace
{
bool g_diagnosticsEnabled = false;

void LogImpl(bool diagnosticOnly, const char* subsystem, const char* format, va_list args)
{
	if (diagnosticOnly && !g_diagnosticsEnabled)
		return;

	char messageBuffer[768] = {};
	std::vsnprintf(messageBuffer, sizeof(messageBuffer), format, args);

	char outputBuffer[1024] = {};
	std::snprintf(outputBuffer, sizeof(outputBuffer), "ToyStory2Fix:%s: %s",
		(subsystem != nullptr && subsystem[0] != '\0') ? subsystem : "Core",
		messageBuffer);
	OutputDebugStringA(outputBuffer);
}
} // namespace

namespace ts2fix
{
void SetDiagnosticsEnabled(bool enabled)
{
	g_diagnosticsEnabled = enabled;
}

bool IsDiagnosticsEnabled()
{
	return g_diagnosticsEnabled;
}

void Log(const char* subsystem, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	LogImpl(false, subsystem, format, args);
	va_end(args);
}

void LogDiagnostic(const char* subsystem, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	LogImpl(true, subsystem, format, args);
	va_end(args);
}
} // namespace ts2fix
