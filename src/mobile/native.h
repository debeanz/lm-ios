#pragma once
// What the phone layer needs from the OS (native_ios.mm on iOS, native_stub.cpp elsewhere). Every
// function is safe to call on any platform; the stubs return "unknown" values.
#include <filesystem>
#include <functional>
#include <string>

namespace mobile::native {

bool isIOS();

// Taptic Engine: a short tap (0 light, 1 medium, 2 heavy) for on-screen buttons, and the game's
// rumble motor (on until rumble(false)).
void haptic(int style);
void rumble(bool on);

// ProMotion panels pick their refresh rate from what the app asks for: a display link requesting
// exactly `hz` (60 or 120) runs the panel at that rate, so every presented frame lands on a refresh
// (presents on a timer at 60 while the panel ran at 120 were shown for 1 or 3 refreshes: judder).
// 0 = no request (the system decides). Returns the rate in effect (0 if unknown).
int setDisplayRate(int hz);

// Device facts for the log header and the [perf] lines; -1 / empty when unknown.
int thermalState();              // 0 nominal, 1 fair, 2 serious, 3 critical
long long footprintMB();         // what iOS counts toward the app's memory limit (incl. GPU)
long long availableMemoryMB();   // left before that limit
long long physicalMemoryMB();
long long freeDiskMB();
bool lowPowerMode();
std::string deviceModel();       // e.g. "iPhone16,2"
std::string osVersion();         // e.g. "iOS 18.1"
std::string appVersion();        // CFBundleShortVersionString (CFBundleVersion)
std::string gpuName();
int maxRefreshRate();            // 120 on ProMotion panels, 60 otherwise; 0 unknown

// Folders. documents: visible in the Files app (On My iPhone > Luigi's Mansion): settings, saves,
// logs, disc images. caches: shader/pipeline caches (iOS may clear them when space runs low).
std::filesystem::path documentsDir();
std::filesystem::path cachesDir();
std::filesystem::path bundleDir();

// Sends stdout/stderr (every printf of the game, Aurora's log, crash reports) through the session
// log (session_log.cpp). iOS: also mirrored to the system log (Console.app).
void mirrorToSystemLog(const char* line, size_t len);

// Memory warnings and app lifecycle from UIKit (in addition to SDL's events).
void installObservers(std::function<void(const char* what)> onEvent);

// The Files picker for a disc image. iOS copies the chosen file into the app, then it is moved into
// documentsDir() and its path handed to the callback (empty: cancelled or failed; error says why).
// The callback runs on the main thread. False: no picker on this platform.
bool pickDiscImage(std::function<void(const std::filesystem::path& picked, const std::string& error)> done);

}  // namespace mobile::native
