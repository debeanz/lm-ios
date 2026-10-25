#pragma once
// Detailed session log on the phone (docs/IOS.md "Logs"). Everything the game prints (stdout and
// stderr: the port's own lines, Aurora's log, hang reports, crash stacks) goes into
// Documents/Logs/session_<yyyyMMdd_HHmmss>.log with a wall-clock time and the seconds since start
// on every line. Saved continuously, line by line (no buffering in the app), so a crash or iOS
// killing the app in the background loses nothing; the last 10 sessions are kept. The folder is
// visible in the Files app (On My iPhone > Luigi's Mansion > Logs).
#include <cstdarg>
#include <filesystem>

namespace mobile::sessionlog {

// First thing in main (before anything prints). iOS only; elsewhere nothing happens unless
// LM_SESSION_LOG=1 (PC test of the phone layer: the console keeps working, the file is a copy).
void start(const std::filesystem::path& dir);
bool active();
std::filesystem::path path();
std::filesystem::path previousPath();  // the session before this one (may hold a crash report)

// One "[category] message" line (also usable before start: then it is only printed).
void line(const char* category, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// Waits until every line printed so far is in the file (lifecycle events, before a planned exit).
void flush();

// Crash path (async-signal-safe): stdout/stderr go straight into the file from here on.
void crashMode();

}  // namespace mobile::sessionlog
