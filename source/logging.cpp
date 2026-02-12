#include "stdafx.h"
#include "ts2fix/logging.h"

#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
bool g_diagnosticsEnabled = false;

std::once_flag g_logInitOnce;
std::mutex g_logMutex;
std::FILE* g_logFile = nullptr;

std::string GetExeDirectory()
{
	char modulePath[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) == 0)
		return {};

	std::string path = modulePath;
	const std::size_t slash = path.find_last_of("\\/");
	if (slash == std::string::npos)
		return {};
	return path.substr(0, slash + 1);
}

bool EnsureDirectoryExists(const std::string& directory)
{
	if (directory.empty())
		return false;

	const DWORD attrs = GetFileAttributesA(directory.c_str());
	if (attrs != INVALID_FILE_ATTRIBUTES)
		return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

	if (CreateDirectoryA(directory.c_str(), nullptr))
		return true;

	return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string GetTempDirectory()
{
	char tempPath[MAX_PATH] = {};
	const DWORD written = GetTempPathA(MAX_PATH, tempPath);
	if (written == 0 || written >= MAX_PATH)
		return {};
	return tempPath;
}

std::FILE* TryOpenLogFile(const std::string& path)
{
	if (path.empty())
		return nullptr;

	return std::fopen(path.c_str(), "a");
}

void InitializeLogFile()
{
	const std::string exeDir = GetExeDirectory();
	if (!exeDir.empty())
	{
		const std::string scriptsDir = exeDir + "scripts";
		if (EnsureDirectoryExists(scriptsDir))
			g_logFile = TryOpenLogFile(scriptsDir + "\\ToyStory2Fix.log");

		if (g_logFile == nullptr)
			g_logFile = TryOpenLogFile(exeDir + "ToyStory2Fix.log");
	}

	if (g_logFile == nullptr)
	{
		const std::string tempDir = GetTempDirectory();
		g_logFile = TryOpenLogFile(tempDir + "ToyStory2Fix.log");
	}

	if (g_logFile != nullptr)
	{
		std::fputs("=== ToyStory2Fix log started ===\n", g_logFile);
		std::fflush(g_logFile);
	}
}

void WriteLogLineToFile(const char* line)
{
	std::call_once(g_logInitOnce, InitializeLogFile);
	if (g_logFile == nullptr || line == nullptr)
		return;

	std::lock_guard<std::mutex> lock(g_logMutex);
	std::fputs(line, g_logFile);
	const std::size_t len = std::strlen(line);
	if (len == 0 || line[len - 1] != '\n')
		std::fputc('\n', g_logFile);
	std::fflush(g_logFile);
}

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
	WriteLogLineToFile(outputBuffer);
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
