// Declarations shared between the shell's own translation units. Not part of the public API.
#pragma once

#include "Shell.h"

#include <string>

namespace AppleShell::Internal
{
// Logging (ShellLog.cpp)
void OpenLog(const std::string& path, const std::string& previous_path);
void CloseLog();
void RedirectSDLLog();
void RestoreSDLLog();

// Crash capture (ShellLog.cpp). Signal handlers append a report to `crash_path`.
void InstallCrashHandler(const std::string& crash_path);
// Appends a text blob to the crash file (used for uncaught Objective-C exceptions and std::terminate).
void WriteCrashText(const char* text);
// Extra line included in every crash report, e.g. the app version. Copied; call from Init.
void SetCrashContext(const std::string& context);

// Event history (Lifecycle.mm)
void RecordEvent(const char* category, const std::string& text);

// Lifecycle (Lifecycle.mm)
void StartObservers();
void StopObservers();
void PollMemoryBudget();
// Called by Shell.mm to apply state changes the session file cares about.
bool IsInBackground();

// Platform helpers (DeviceInfo.mm / Shell.mm)
double SecondsSinceInit();

// Session state file (Shell.mm): "running" | "background" | "clean"
void WriteSessionState(const char* state);
} // namespace AppleShell::Internal
