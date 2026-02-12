#pragma once

namespace ts2fix
{
void SetDiagnosticsEnabled(bool enabled);
bool IsDiagnosticsEnabled();

void Log(const char* subsystem, const char* format, ...);
void LogDiagnostic(const char* subsystem, const char* format, ...);
} // namespace ts2fix
