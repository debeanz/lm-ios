#include "recomp/cpu_qos.h"

#include "recomp/hle.h"

#include <aurora/aurora.h>
#include <aurora/thread_hooks.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <intrin.h>
#define LM_CPU_X86 1
#endif
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace recomp::cpu {
namespace {

constexpr int kRoles = int(Role::Count);
const char* const kRoleName[kRoles] = {"host", "render worker", "FIFO", "guest", "timer", "audio"};

bool sEnabled = false;
bool sHybrid = false;
int sSets = 0;  // hybrid: 0 = performance cores, 1 = no CPU set, 2 = efficiency cores (test aid)
double sLogSecs = 60.0;

// Core type of each logical processor: [group * 64 + number] -> 1 = performance, 2 = efficiency.
constexpr int kMaxProcs = 4 * 64;
unsigned char sKind[kMaxProcs] = {};

thread_local int tRole = -1;
std::atomic<uint64_t> sOnP[kRoles], sOnE[kRoles];

#ifdef _WIN32
std::vector<ULONG> sPerfSets, sEffSets;

// (Declared here: older SDK headers lack the power-throttling structures and classes.)
struct PowerThrottlingState {
    ULONG Version, ControlMask, StateMask;
};
constexpr ULONG kThrottleExecutionSpeed = 0x1, kThrottleIgnoreTimerResolution = 0x4;
constexpr int kProcessPowerThrottling = 4;  // PROCESS_INFORMATION_CLASS
constexpr int kThreadPowerThrottling = 3;   // THREAD_INFORMATION_CLASS

using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using SetThreadInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using SetThreadSelectedCpuSetsFn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);
using GetSystemCpuSetInformationFn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
SetThreadInformationFn sSetThreadInformation = nullptr;
SetThreadSelectedCpuSetsFn sSetThreadSelectedCpuSets = nullptr;

std::string brand() {
#ifdef LM_CPU_X86
    int r[4] = {};
    __cpuid(r, 0x80000000);
    if (unsigned(r[0]) < 0x80000004u) return "unknown CPU";
    char s[49] = {};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, 0x80000002 + i);
        std::memcpy(s + i * 16, r, 16);
    }
    std::string b(s);
    const size_t a = b.find_first_not_of(' ');
    return a == std::string::npos ? "unknown CPU" : b.substr(a);
#else
    return "non-x86 CPU";
#endif
}

void detect() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const auto getSets = k32 ? reinterpret_cast<GetSystemCpuSetInformationFn>(GetProcAddress(k32, "GetSystemCpuSetInformation")) : nullptr;
    sSetThreadSelectedCpuSets = k32 ? reinterpret_cast<SetThreadSelectedCpuSetsFn>(GetProcAddress(k32, "SetThreadSelectedCpuSets")) : nullptr;
    sSetThreadInformation = k32 ? reinterpret_cast<SetThreadInformationFn>(GetProcAddress(k32, "SetThreadInformation")) : nullptr;
    struct Set {
        ULONG id;
        WORD group;
        BYTE index, cls;
    };
    std::vector<Set> sets;
    if (getSets) {
        ULONG len = 0;
        getSets(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<unsigned char> buf(len);
        if (len && getSets(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()), len, &len, GetCurrentProcess(), 0)) {
            for (ULONG off = 0; off + sizeof(DWORD) * 2 <= len;) {
                const auto* e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
                if (e->Size == 0) break;
                if (e->Type == CpuSetInformation)
                    sets.push_back({e->CpuSet.Id, e->CpuSet.Group, e->CpuSet.LogicalProcessorIndex, e->CpuSet.EfficiencyClass});
                off += e->Size;
            }
        }
    }
    BYTE lo = 255, hi = 0;
    for (const Set& s : sets) lo = std::min(lo, s.cls), hi = std::max(hi, s.cls);
    sHybrid = !sets.empty() && hi != lo;
    // A higher efficiency class is a faster core; the performance cores are the highest class
    // (on CPUs with three tiers, the middle tier counts as efficiency here).
    for (const Set& s : sets) {
        const bool perf = !sHybrid || s.cls == hi;
        (perf ? sPerfSets : sEffSets).push_back(s.id);
        const int slot = int(s.group) * 64 + int(s.index);
        if (slot < kMaxProcs) sKind[slot] = perf ? 1 : 2;
    }
    if (sHybrid)
        log("[cpu] %s: %zu logical processors, hybrid: %zu on performance cores (class %u), %zu on efficiency cores",
            brand().c_str(), sets.size(), sPerfSets.size(), unsigned(hi), sEffSets.size());
    else
        log("[cpu] %s: %zu logical processors, not hybrid", brand().c_str(), sets.size());
}

void processPolicy() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    const auto setProc = k32 ? reinterpret_cast<SetProcessInformationFn>(GetProcAddress(k32, "SetProcessInformation")) : nullptr;
    // No EcoQoS for the process (Windows 11 lowers the clock and prefers the efficiency cores for
    // throttled processes, e.g. while the window is not in the foreground), and timer resolution
    // requests (timeBeginPeriod) are honoured even then.
    PowerThrottlingState st{1, kThrottleExecutionSpeed | kThrottleIgnoreTimerResolution, 0};
    const bool ok = setProc && setProc(GetCurrentProcess(), kProcessPowerThrottling, &st, sizeof(st));
    log("[cpu] power throttling (EcoQoS) for the process: %s", ok ? "off" : "unchanged (not supported)");
}
#endif

void startHook(const char* name) {
    if (!name) return;
    if (!std::strcmp(name, "Main thread")) setCurrent(Role::Host);
    else if (!std::strcmp(name, "Aurora render worker")) setCurrent(Role::Render);
    else if (!std::strcmp(name, "Aurora FIFO processor")) setCurrent(Role::Fifo);
}

}  // namespace

void init() {
    const char* q = std::getenv("LM_CPU_QOS");
    sEnabled = !(q && *q == '0');
    if (const char* s = std::getenv("LM_CPU_SETS")) sSets = *s == 'a' ? 1 : *s == 'e' ? 2 : 0;
    if (const char* l = std::getenv("LM_CPU_LOG")) sLogSecs = std::atof(l);
    // (Also with the policy off: the hooks sample which cores the threads run on.)
    aurora_set_thread_hooks(startHook, tick);
#ifdef _WIN32
    detect();
    if (!sEnabled) {
        log("[cpu] thread policy off (LM_CPU_QOS=0)");
        return;
    }
    processPolicy();
    if (sHybrid)
        log("[cpu] time-critical threads: %s", sSets == 0 ? "performance cores only" : sSets == 1 ? "any core (LM_CPU_SETS=any)"
                                                                                      : "efficiency cores only (LM_CPU_SETS=e, test)");
#endif
}

void profileRegister(int role);  // cpu_profile.cpp (test aid LM_PROFILE)

void setCurrent(Role role) {
    tRole = int(role);
    profileRegister(int(role));
#ifdef _WIN32
    if (!sEnabled) return;
    const HANDLE self = GetCurrentThread();
    int prio = THREAD_PRIORITY_ABOVE_NORMAL;
    if (role == Role::Host || role == Role::Timer || role == Role::Audio) prio = THREAD_PRIORITY_HIGHEST;
    const bool prioOk = SetThreadPriority(self, prio) != 0;
    PowerThrottlingState st{1, kThrottleExecutionSpeed, 0};
    const bool qosOk = sSetThreadInformation && sSetThreadInformation(self, kThreadPowerThrottling, &st, sizeof(st));
    bool setOk = false;
    const std::vector<ULONG>* sets = nullptr;
    if (sHybrid && sSets != 1 && sSetThreadSelectedCpuSets) {
        sets = sSets == 2 ? &sEffSets : &sPerfSets;
        setOk = !sets->empty() && sSetThreadSelectedCpuSets(self, sets->data(), ULONG(sets->size()));
    }
    // One line per role (the guest threads are many and alike).
    static std::atomic<unsigned> sLogged{0};
    if (!(sLogged.fetch_or(1u << unsigned(role)) & (1u << unsigned(role))))
        log("[cpu] %s thread: priority %s%s, power throttling %s%s", kRoleName[int(role)],
            prio == THREAD_PRIORITY_HIGHEST ? "highest" : "above normal", prioOk ? "" : " (failed)", qosOk ? "off" : "unchanged",
            sets ? (setOk ? (sSets == 2 ? ", efficiency cores" : ", performance cores") : ", CPU set failed") : "");
#elif defined(__APPLE__)
    // iPhone / iPad (docs/IOS.md): Apple chips have performance and efficiency cores too, and a
    // thread at the default QoS may be scheduled on the efficiency cores (the game thread at about
    // a third of its speed: fast-forward at 2x instead of 30x, hitches in play). Every role here is
    // time-critical: user-interactive, the class of the UI thread itself.
    if (!sEnabled) return;
    const int err = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    static std::atomic<unsigned> sLogged{0};
    if (!(sLogged.fetch_or(1u << unsigned(role)) & (1u << unsigned(role))))
        log("[cpu] %s thread: QoS user-interactive%s", kRoleName[int(role)],
            err ? " not applied (the thread already has a fixed priority of its own, e.g. SDL's high priority)" : "");
#endif
}

void tick() {
    const int r = tRole;
    if (r < 0 || !sHybrid) return;
#ifdef _WIN32
    PROCESSOR_NUMBER pn;
    GetCurrentProcessorNumberEx(&pn);
    const int slot = int(pn.Group) * 64 + int(pn.Number);
    if (slot >= kMaxProcs) return;
    (sKind[slot] == 2 ? sOnE : sOnP)[r].fetch_add(1, std::memory_order_relaxed);
#endif
}

void report() {
    tick();
    const auto now = std::chrono::steady_clock::now();
    // Window focus changes: a background window may be scheduled differently (issue #18).
    static int sFocus = -1;
    if (SDL_Window* w = aurora_get_window()) {
        const int f = (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0;
        if (f != sFocus) {
            if (sFocus >= 0) log("[cpu] window %s the focus", f ? "has" : "lost");
            sFocus = f;
        }
    }
    if (!sHybrid || sLogSecs <= 0) return;
    static auto sLast = now;
    if (std::chrono::duration<double>(now - sLast).count() < sLogSecs) return;
    sLast = now;
    char line[512];
    int n = std::snprintf(line, sizeof(line), "[cpu] samples on performance cores in the last %.0f s:", sLogSecs);
    for (int r = 0; r < kRoles && n < int(sizeof(line)) - 48; r++) {
        const uint64_t p = sOnP[r].exchange(0), e = sOnE[r].exchange(0);
        if (p + e == 0) continue;
        n += std::snprintf(line + n, sizeof(line) - size_t(n), " %s %.1f%%", kRoleName[r], 100.0 * double(p) / double(p + e));
    }
    log("%s", line);
}

}  // namespace recomp::cpu
