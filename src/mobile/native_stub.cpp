// Phone layer on a PC (LM_MOBILE=1 test mode): no Taptic Engine, no Files picker; folders are the
// port's usual ones (native.h).
#ifndef __APPLE__
#include "mobile/native.h"

#include "port/install.h"

#include <cstdio>

namespace mobile::native {

bool isIOS() { return false; }
void haptic(int) {}
void rumble(bool) {}
int startRefreshSignal(int, void (*)()) { return 0; }
const std::vector<unsigned char>& systemFont(std::string& name) {
    static const std::vector<unsigned char> sNone;
    name = "no system font on this platform (ImGui's default font)";
    return sNone;
}
int thermalState() { return -1; }
long long footprintMB() { return -1; }
long long availableMemoryMB() { return -1; }
long long physicalMemoryMB() { return -1; }
long long freeDiskMB() { return -1; }
bool lowPowerMode() { return false; }
std::string deviceModel() { return "PC (phone layer test mode)"; }
std::string osVersion() {
#ifdef _WIN32
    return "Windows";
#else
    return "Linux";
#endif
}
std::string appVersion() { return {}; }
std::string gpuName() { return {}; }
int maxRefreshRate() { return 0; }
std::filesystem::path documentsDir() { return port::dataDir(); }
std::filesystem::path cachesDir() { return port::cacheDir(); }
std::filesystem::path bundleDir() { return {}; }
void mirrorToSystemLog(const char*, size_t) {}
void installObservers(std::function<void(const char*)>) {}
bool pickDiscImage(std::function<void(const std::filesystem::path&, const std::string&)>) { return false; }

}  // namespace mobile::native
#endif
