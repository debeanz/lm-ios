#include "recomp/pacing.h"

#include "mobile/mobile.h"
#include "mobile/native.h"
#include "port/settings.h"
#include "recomp/cpu_qos.h"
#include "recomp/frame.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/skip.h"

#include <aurora/aurora.h>
#include <aurora/present_timing.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace recomp::pacing {
namespace {

double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
Clock::duration dur(double s) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); }

// --- vblank monitor (Windows: D3DKMTWaitForVerticalBlankEvent on the window's display) -----------
// Gives the display's refresh times: the fixed-refresh schedule starts frames at vblanks, and the
// spacing of refreshes tells a variable refresh rate display (refreshes follow the presents) from a
// fixed one (refreshes are multiples of the period).
struct Vblank {
    std::mutex m;
    std::condition_variable cv;
    u64 count = 0;
    Clock::time_point last{};
    std::deque<double> intervals;  // ms, newest last
    std::atomic<bool> running{false};
    std::thread th;
    std::wstring device;          // requested by the host thread
    std::atomic<bool> deviceChanged{false};
};
Vblank sVb;

#ifdef _WIN32
struct KmtOpen {
    WCHAR DeviceName[32];
    UINT hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct KmtWait {
    UINT hAdapter;
    UINT hDevice;
    UINT VidPnSourceId;
};
struct KmtClose {
    UINT hAdapter;
};
using OpenFn = LONG(APIENTRY*)(KmtOpen*);
using WaitFn = LONG(APIENTRY*)(const KmtWait*);
using CloseFn = LONG(APIENTRY*)(const KmtClose*);

void vblankThread() {
    cpu::setCurrent(cpu::Role::Timer);
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    const auto open = gdi ? reinterpret_cast<OpenFn>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName")) : nullptr;
    const auto wait = gdi ? reinterpret_cast<WaitFn>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent")) : nullptr;
    const auto close = gdi ? reinterpret_cast<CloseFn>(GetProcAddress(gdi, "D3DKMTCloseAdapter")) : nullptr;
    if (!open || !wait) return;
    KmtWait w{};
    bool opened = false;
    while (sVb.running) {
        if (!opened || sVb.deviceChanged.exchange(false)) {
            if (opened && close) {
                KmtClose c{w.hAdapter};
                close(&c);
            }
            opened = false;
            KmtOpen o{};
            {
                std::lock_guard lk(sVb.m);
                wcsncpy_s(o.DeviceName, sVb.device.c_str(), 31);
                sVb.intervals.clear();
                sVb.last = {};
            }
            if (o.DeviceName[0] && open(&o) == 0) {
                w = {o.hAdapter, 0, o.VidPnSourceId};
                opened = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
        }
        if (wait(&w) != 0) {
            opened = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const auto now = Clock::now();
        {
            std::lock_guard lk(sVb.m);
            if (sVb.last != Clock::time_point{}) {
                sVb.intervals.push_back(secs(now - sVb.last) * 1000.0);
                if (sVb.intervals.size() > 360) sVb.intervals.pop_front();
            }
            sVb.last = now;
            sVb.count++;
        }
        sVb.cv.notify_all();
    }
    if (opened && close) {
        KmtClose c{w.hAdapter};
        close(&c);
    }
}

// The GDI name (\\.\DISPLAYn) of the display the window is on.
std::wstring windowDevice() {
    SDL_Window* win = aurora_get_window();
    if (!win) return {};
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return {};
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return {};
    return mi.szDevice;
}
#endif

// Called on the host thread now and then: (re)starts the monitor on the window's display.
void vblankFollowWindow() {
#ifdef _WIN32
    static const bool sOff = std::getenv("LM_NO_VBLANK") != nullptr;
    if (sOff) return;
    static Clock::time_point checked{};
    const auto now = Clock::now();
    if (checked != Clock::time_point{} && now - checked < std::chrono::milliseconds(500)) return;
    checked = now;
    const std::wstring dev = windowDevice();
    {
        std::lock_guard lk(sVb.m);
        if (dev == sVb.device) return;
        sVb.device = dev;
    }
    sVb.deviceChanged = true;
    if (!sVb.running.exchange(true)) {
        sVb.th = std::thread(vblankThread);
        sVb.th.detach();
    }
#endif
}

bool vblankAvailable() {
    std::lock_guard lk(sVb.m);
    return sVb.count > 0 && Clock::now() - sVb.last < std::chrono::milliseconds(200);
}

}  // namespace

// iPhone / iPad (src/mobile): a display link on its own thread calls this at every refresh of the
// panel, the same signal the Windows monitor above gives (fixed-refresh schedule: 60 fps on a
// 120 Hz ProMotion panel starts a frame at every second refresh, so each is shown for exactly two).
void displayRefreshed() {
    {
        std::lock_guard lk(sVb.m);
        const auto now = Clock::now();
        if (sVb.last != Clock::time_point{}) {
            sVb.intervals.push_back(secs(now - sVb.last) * 1000.0);
            while (sVb.intervals.size() > 240) sVb.intervals.pop_front();
        }
        sVb.last = now;
        sVb.count++;
    }
    sVb.cv.notify_all();
}

namespace {

// --- frame cost --------------------------------------------------------------------------------
struct FrameRec {
    u64 frame = 0;
    Clock::time_point begun{}, ended{};  // after aurora_begin_frame / after aurora_end_frame (host)
    double gpuDone = 0;                   // s since epoch (steady clock), 0 = not yet
    // LM_PACING_LOG
    Clock::time_point enter{}, sched{}, woke{}, started{}, opened{};
    double need = 0, period = 0, target = 0;
    char mode = '-';
    u64 tape = 0, newestTape = 0;
    float alpha = 1;
    double newestDone = 0;
    bool stalled = false;
    double gpuCost = 0;
    u32 warm = 0;  // GPU keep-warm units when the frame was built
    double held = 0;  // s: beforeEnd's hold
    Clock::time_point replayed{}, ending{};  // after the game frame / interpolated replay; at beforeEnd
};
FrameRec sCur;  // the frame being built
std::deque<FrameRec> sFrames;     // recent frames, oldest first
std::deque<double> sCost;         // per frame: the time it occupied the busiest stage (s)
std::deque<double> sCostNoHold;   // the same without beforeEnd's hold, last 15 frames (governor floor)
std::deque<double> sGpuCost;      // per frame: the GPU's time on it (s)
double sLastDone = 0;

double nsToSec(int64_t ns) { return double(ns) * 1e-9; }
double tpSec(Clock::time_point t) { return secs(t.time_since_epoch()); }

// --- LM_PACING_LOG=<file>: one line per presented frame + a summary per second ------------------
FILE* sLog = [] {
    const char* p = std::getenv("LM_PACING_LOG");
    FILE* f = p && *p ? std::fopen(p, "w") : nullptr;
    if (f) {
        std::fprintf(f,
                     "# F frame mode target_ms need_ms period_ms | enter sched woke started opened ended submit present "
                     "done (ms since start) | cost_ms cpu_ms gpu_ms | tape newest_tape newest_age_ms alpha stalled warm_units replay_ms "
                     "menu_ms hold_ms endframe_ms\n"
                     "# mode: G grid, V vblank, F fifo (swap chain paces), U unlimited, L lockstep (not interpolating)\n"
                     "# enter=beforeFrame entered, sched=scheduled start, woke=after the schedule wait, started=after "
                     "the GPU wait, opened=after aurora_begin_frame, ended=after aurora_end_frame\n"
                     "# S second presents fps_present fps_done max_iv_ms(present/done) over1.5x(present/done) "
                     "cost_avg/max need_avg period_avg wait_sched_avg wait_gpu_avg begin_avg cpu_avg stalls modes\n");
    }
    return f;
}();
const Clock::time_point sLogT0 = Clock::now();
double logMs(double sec) { return sec ? (sec - secs(sLogT0.time_since_epoch())) * 1000.0 : 0.0; }
double logMs(Clock::time_point t) { return t == Clock::time_point{} ? 0.0 : secs(t - sLogT0) * 1000.0; }

struct Summary {
    double start = 0;  // s (log time)
    u32 n = 0, stalls = 0, overP = 0, overD = 0;
    double lastPresent = 0, lastDone = 0, maxP = 0, maxD = 0;
    double firstP = 0, lastP = 0, firstD = 0, lastD = 0;
    u32 nP = 0, nD = 0;
    double cost = 0, costMax = 0, need = 0, period = 0, wSched = 0, wGpu = 0, begin = 0, cpu = 0;
    std::string modes;
    double target = 0;
};
Summary sSum;

void logFrame(const FrameRec& r, const AuroraPresentTiming& t, double cost, double cpu, double gpu) {
    if (!sLog) return;
    const double present = t.presentNs ? nsToSec(t.presentNs) : 0.0, done = t.doneNs ? nsToSec(t.doneNs) : 0.0;
    std::fprintf(sLog,
                 "F %llu %c %.3f %.3f %.3f | %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | %.3f %.3f %.3f | %llu %llu %.2f "
                 "%.3f %d %u %.3f %.3f %.3f %.3f\n",
                 (unsigned long long)r.frame, r.mode, r.target * 1e3, r.need * 1e3, r.period * 1e3, logMs(r.enter), logMs(r.sched),
                 logMs(r.woke), logMs(r.started), logMs(r.opened), logMs(r.ended), logMs(t.submitNs ? nsToSec(t.submitNs) : 0.0),
                 logMs(present), logMs(done), cost * 1e3, cpu * 1e3, gpu * 1e3, (unsigned long long)r.tape,
                 (unsigned long long)r.newestTape, r.newestDone ? (tpSec(r.opened) - r.newestDone) * 1e3 : 0.0, r.alpha,
                 r.stalled ? 1 : 0, r.warm, r.replayed != Clock::time_point{} ? secs(r.replayed - r.opened) * 1e3 : 0.0,
                 r.ending != Clock::time_point{} && r.replayed != Clock::time_point{} ? secs(r.ending - r.replayed) * 1e3 : 0.0,
                 r.held * 1e3, r.ending != Clock::time_point{} ? secs(r.ended - r.ending) * 1e3 - r.held * 1e3 : 0.0);
    // Summary per second of log time.
    Summary& s = sSum;
    const double now = secs(r.ended - sLogT0);
    if (s.start == 0) s.start = now;
    const double limit = r.target * 1.5;
    if (present) {
        if (s.lastPresent) {
            const double iv = present - s.lastPresent;
            s.maxP = std::max(s.maxP, iv);
            if (r.target > 0 && iv > limit) s.overP++;
        }
        if (!s.nP++) s.firstP = present;
        s.lastP = present;
        s.lastPresent = present;
    }
    if (done) {
        if (s.lastDone) {
            const double iv = done - s.lastDone;
            s.maxD = std::max(s.maxD, iv);
            if (r.target > 0 && iv > limit) s.overD++;
        }
        if (!s.nD++) s.firstD = done;
        s.lastD = done;
        s.lastDone = done;
    }
    s.n++;
    s.stalls += r.stalled ? 1 : 0;
    s.cost += cost;
    s.costMax = std::max(s.costMax, cost);
    s.need += r.need;
    s.period += r.period;
    s.wSched += secs(r.woke - r.enter);
    s.wGpu += secs(r.started - r.woke);
    s.begin += secs(r.opened - r.started);
    s.cpu += cpu;
    s.target = r.target;
    if (s.modes.find(r.mode) == std::string::npos) s.modes += r.mode;
    if (now - s.start >= 1.0) {
        const double fp = s.nP > 1 ? double(s.nP - 1) / (s.lastP - s.firstP) : 0.0;
        const double fd = s.nD > 1 ? double(s.nD - 1) / (s.lastD - s.firstD) : 0.0;
        const double k = 1000.0 / double(s.n);
        std::fprintf(sLog, "S %.1f %u %.2f %.2f %.2f/%.2f %u/%u %.2f/%.2f %.2f %.2f %.2f %.2f %.2f %.2f %u %s target %.2f\n",
                     now, s.n, fp, fd, s.maxP * 1e3, s.maxD * 1e3, s.overP, s.overD, s.cost * k, s.costMax * 1e3, s.need * k,
                     s.period * k, s.wSched * k, s.wGpu * k, s.begin * k, s.cpu * k, s.stalls, s.modes.c_str(),
                     s.target * 1e3);
        std::fflush(sLog);
        const double lp = s.lastPresent, ld = s.lastDone;
        s = Summary{};
        s.lastPresent = lp;
        s.lastDone = ld;
    }
}

// --- GPU keep-warm (Aurora 0215) ---------------------------------------------------------------
// A light frame (the pause screen or the Game Boy Horror over a still picture: ~0.5 ms of GPU work)
// lets the GPU fall to its idle clocks (RTX 3090 Ti: 345-450 MHz, P5/P8, measured with nvidia-smi);
// the first 3 frames of the full scene after it then took 45-60 ms instead of 6 (4K, enhanced
// lighting Ultra). Filler work keeps the GPU about as busy as in play: `heavy` follows the GPU
// time of recent unfilled frames (held, halving over 20 s); the filler grows while a frame takes
// less than 75% of want = min(heavy, 90% of the frame period), eases off above 115% and stops at
// once above 160% (the full scene is back). The first full frame after a menu costs its own time
// plus the filler. At 120 fps lower caps did not hold the clocks (2-minute menu cycling: 40% ->
// 20-28 ms frames after each menu, 60% -> 69 intervals over 1.5x, 90% -> 14); at 60 fps `heavy`
// is the binding limit anyway.
struct KeepWarm {
    double heavy = 0;   // s
    u32 units = 0;
} sWarm;

void setWarmUnits(KeepWarm& w, u32 u) {
    // (Logged so a report shows whether the filler ran: the first start, then at most every 60 s
    // how often it started since; it can start and stop several times a second over a menu.)
    if (w.units == 0 && u != 0) {
        static u32 sStarts = 0;
        static Clock::time_point sLogged{};
        const auto now = Clock::now();
        sStarts++;
        if (sLogged == Clock::time_point{} || now - sLogged > std::chrono::seconds(60)) {
            log("[pacing] GPU keep-warm running (%u starts since the last line; %u units, heavy %.1f ms)", sStarts, u,
                w.heavy * 1e3);
            sStarts = 0;
            sLogged = now;
        }
    }
    aurora_set_gpu_keep_warm(w.units = u);
}

void keepWarm(double gpu, double period) {
    KeepWarm& w = sWarm;
    if (!keepWarmOn() || gpu <= 0 || period <= 0) {
        if (w.units) setWarmUnits(w, 0);
        return;
    }
    // (Held while filling: a long menu must not lower the level the GPU is kept at; measured at
    // 120 fps the filler fell to 40% of the period over 5 s and the clocks dropped anyway.)
    if (w.units == 0) w.heavy *= std::pow(0.5, period / 20.0);
    // (A frame longer than the period is a hitch or an overload, not the level to hold.)
    if (w.units == 0 && gpu < period) w.heavy = std::max(w.heavy, gpu);
    // LM_KEEP_WARM_CAP=<fraction> (test aid): the cap as a fraction of the period (default 0.9).
    static const double sCap = [] { const char* e = std::getenv("LM_KEEP_WARM_CAP"); return e ? std::atof(e) : 0.9; }();
    const double want = std::min(w.heavy, sCap * period);
    u32 u = w.units;
    if (w.heavy < 0.003) u = 0;  // light settings: the GPU stays fast enough at any clock
    else if (gpu > period * 0.9 || gpu > want * 1.6) u = 0;  // the full scene is back (or too much)
    else if (gpu > want * 1.15) u = u * 7 / 8;
    else if (gpu < want * 0.75) u = u == 0 ? 2 : std::min<u32>(u + u / 5 + 1, 400);
    if (u != w.units) setWarmUnits(w, u);
}

// --- performance summary in the log (last_run.log), issue #18 ------------------------------------
// Every LM_PERF_LOG seconds (default 60, 0 = off) while interpolating: presented and game frame
// rates, present intervals over 1.5x the target, and the per-frame time of each stage (host thread
// without the present hold, render worker, GPU) as median / 95th percentile / max, plus the
// governor's slowdowns. A player's log then shows which stage misses the budget.
struct Perf {
    std::vector<float> host, worker, gpu;  // ms
    double lastPresent = 0, first = 0, last = 0;
    u32 presents = 0, over = 0;
} sPerf;

void perfFrame(const FrameRec& r, const AuroraPresentTiming& t, double cpu, double worker, double gpu) {
    if (r.mode == 'L') return;
    Perf& p = sPerf;
    p.host.push_back(float(std::max(0.0, cpu - r.held) * 1e3));
    p.worker.push_back(float(worker * 1e3));
    p.gpu.push_back(float(gpu * 1e3));
    if (t.presentNs) {
        const double pr = nsToSec(t.presentNs);
        if (p.lastPresent && r.target > 0 && pr - p.lastPresent > 1.5 * r.target) p.over++;
        if (!p.presents++) p.first = pr;
        p.last = pr;
        p.lastPresent = pr;
    }
}

void pollTimings() {
    AuroraPresentTiming t[32];
    for (size_t n; (n = aurora_present_timings(t, 32)) > 0;) {
        for (size_t i = 0; i < n; i++) {
            interp::logPresentTiming(t[i].frame, t[i].submitNs, t[i].presentNs, t[i].doneNs);
            if (!t[i].doneNs) continue;
            const double done = nsToSec(t[i].doneNs);
            for (FrameRec& r : sFrames) {
                if (r.frame != t[i].frame) continue;
                r.gpuDone = done;
                // Throughput per stage (the stages overlap: host CPU, render worker, GPU): the
                // host from the frame's start (after the schedule and GPU waits) to end_frame, the
                // render worker from end_frame to its submit, the GPU from the later of that submit
                // and the previous frame's completion to this frame's completion. (The old estimate,
                // done - max(ended, previous done), added the render worker to the GPU.)
                const double submit = t[i].submitNs ? nsToSec(t[i].submitNs) : tpSec(r.ended);
                const double gpu = done - std::max(submit, sLastDone);
                const double cpu = secs(r.ended - r.begun);
                const double worker = std::max(0.0, submit - tpSec(r.ended));
                const double cost = std::max({gpu, cpu, worker});
                r.gpuCost = gpu;
                if (r.mode != 'L') {
                    sCost.push_back(cost);
                    sCostNoHold.push_back(std::max({gpu, cpu - r.held, worker}));
                    if (sCostNoHold.size() > 15) sCostNoHold.pop_front();
                    sGpuCost.push_back(gpu);
                    if (sCost.size() > 30) sCost.pop_front();
                    if (sGpuCost.size() > 30) sGpuCost.pop_front();
                }
                logFrame(r, t[i], cost, cpu, gpu);
                perfFrame(r, t[i], cpu, worker, gpu);
                keepWarm(gpu, r.target > 0 ? r.target : 1.0 / double(frame::targetFps()));
                break;
            }
            sLastDone = std::max(sLastDone, done);
        }
    }
}

// Median of the recent frames' cost (for the status line only: what limits the rate when the
// governor below has slowed it down).
double median(const std::deque<double>& d) {
    if (d.size() < 8) return 0.0;
    std::vector<double> v(d.begin(), d.end());
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// --- rate governor ---------------------------------------------------------------------------------
// Decides whether the PC sustains the target from what actually happens on screen, not from a cost
// estimate. The old rule (period >= p90 of the measured frame cost x 1.08 over 45 frames) failed in
// both directions on the user's PC (RTX 3090 Ti, 4K, enhanced lighting Ultra, 60 fps target):
//  - one hitch (a first-time pipeline compile: 100-670 ms in the render worker) put the p90 at
//    55-85 ms, so the schedule ran at 12-18 fps for seconds until the sample left the window (which
//    takes longer the slower it runs) and then sped up by only 2% per frame;
//  - while slowed down, the GPU clocks down and the measured cost per frame rises (9 ms at 60 fps,
//    20-27 ms at 12 fps), which kept the estimate above the target: a feedback loop.
// Now: a frame whose start comes more than 25% late is a miss; a single very late frame (a hitch:
// a load, a compile) only restarts the schedule. Only sustained misses (a quarter of the last
// second's frames, at least 4) or frames that are a little late most of the time (half of them by
// more than 8%: uneven intervals) lower the rate, to the rate actually achieved; after 1 s without
// misses it speeds up again (2% per frame), not below a rate that failed in the last 10 s.
// Not judged within 1 s of a hitch (the first frames after a load or a room change: new textures,
// shadow maps and lights for the first time, the GPU clocking back up), nor within 1 s of a
// slowdown (the window is judged afresh, not from its first 15 frames): on the first arrival in
// the foyer one burst of uneven frames cut 120 -> 112 -> 107 -> 102 fps within 260 ms, and the
// 10 s floor then held ~100 fps for half a minute while the frames cost 6 ms.
struct Governor {
    double limit = 0;  // s: the frame period the PC sustains (0: the target holds)
    double floor = 0;  // s: a period that failed recently
    Clock::time_point floorUntil{}, holdUntil{}, prevStart{};
    Clock::time_point judgeFrom{};  // no slowdown before this (after a hitch or a slowdown)
    struct Interval {
        Clock::time_point t;
        double iv;   // s
        bool late;   // > 8% over the period it was scheduled at
        bool miss;   // > 25% over it
    };
    std::deque<Interval> intervals;  // the last second's start intervals (no hitches)
    u32 hitches = 0, slowdowns = 0;
};
Governor sGov;

// After this frame's start (all waits done). `expected`: the period it was scheduled at.
void governFrame(Clock::time_point start, double expected, double target, bool sameMode) {
    Governor& g = sGov;
    // A cutscene skip (skip.h) presents one frame per batch of hidden ones: those uneven intervals
    // say nothing about the rate the device holds. The governor sits out the skip and starts afresh
    // after it (on a phone a skip used to leave it limited to 30 fps for ~10 s: lag after a skip).
    static bool sWasFast = false;
    const bool fast = skip::retraceSpeed() > 1.0;
    if (fast || sWasFast) {
        if (!fast && sWasFast) log("[pacing] skip over: governor reset (was %.1f fps)", g.limit > 0 ? 1.0 / g.limit : 0.0);
        sWasFast = fast;
        const u32 hitches = g.hitches, slowdowns = g.slowdowns;
        g = Governor{};
        g.hitches = hitches;
        g.slowdowns = slowdowns;
        g.prevStart = start;
        return;
    }
    const Clock::time_point prev = g.prevStart;
    g.prevStart = start;
    if (!sameMode || prev == Clock::time_point{} || expected <= 0) return;
    const double iv = secs(start - prev);
    while (!g.intervals.empty() && start - g.intervals.front().t > std::chrono::milliseconds(1000)) g.intervals.pop_front();
    if (iv > std::max(3.0 * expected, expected + 0.05)) {
        g.hitches++;  // a load or compile: not a sign the PC cannot hold the rate
        g.judgeFrom = std::max(g.judgeFrom, start + std::chrono::milliseconds(1000));
        g.intervals.clear();
        return;
    }
    // LM_PACING_GRACE=0 (A/B): judge right after hitches and slowdowns again.
    static const bool sGrace = [] {
        const char* v = std::getenv("LM_PACING_GRACE");
        return !(v && std::atoi(v) == 0);
    }();
    if (sGrace && start < g.judgeFrom) {
        g.intervals.clear();  // the window starts after the grace (not with its burst in it)
        return;
    }
    // Each interval is judged against the period it was scheduled at (while speeding up, the
    // period shrinks every frame: judging the last second against the newest one counted on-time
    // frames as late and locked a 90 fps target at 20-45 fps).
    g.intervals.push_back({start, iv, iv > expected * 1.08, iv > expected * 1.25});
    size_t late = 0, misses = 0;
    for (const auto& i : g.intervals) {
        late += i.late ? 1 : 0;
        misses += i.miss ? 1 : 0;
    }
    const size_t n = g.intervals.size();
    // Slightly late most of the time (the frame costs a little more than the period, e.g. 8-11 ms
    // at a 116 fps cap): intervals that alternate between the period and a bit more are uneven
    // motion on a VRR display; an even rate a little lower is smoother.
    const bool chronic = n >= 20 && late * 2 >= n;
    // Sustained: a quarter of the last second's frames (at least 4) late. A few slow frames in a
    // row (a scene switch: new textures, the first frames of a room) are not.
    const bool sustained = n >= 15 && misses >= std::max<size_t>(4, n / 4);
    if (chronic || sustained) {
        // The rate actually achieved over the last half second, plus 3%.
        double sum = 0;
        u32 k = 0;
        for (const auto& i : g.intervals)
            if (start - i.t <= std::chrono::milliseconds(500)) sum += i.iv, k++;
        const double achieved = k ? sum / k : iv;
        const double lim = std::max({achieved * 1.03, expected * 1.05, g.limit});
        log("[pacing] frame rate not sustained (%zu of %zu frames late in 1 s, %zu by >25%%; %.1f ms achieved vs %.1f ms): "
            "limited to %.1f fps",
            late, n, misses, achieved * 1000.0, expected * 1000.0, 1.0 / lim);
        g.limit = lim;
        g.floor = lim * 0.97;
        g.floorUntil = start + std::chrono::seconds(10);
        g.holdUntil = start + std::chrono::milliseconds(1000);
        g.judgeFrom = start + std::chrono::milliseconds(1000);
        g.intervals.clear();  // judged afresh at the new period
        g.slowdowns++;
        return;
    }
    // The 10 s floor guards against slowing down again right after speeding up. When the frames
    // cost far less than the target now (every stage of the last 15 frames' median under 75% of the
    // target period), the cause of the slowdown is gone (a GPU stall, another program's load: issue
    // #18, a 0.4 s stall cut 57 -> 20.5 fps and the floor held ~21 fps for 10 s while the frames cost
    // 3-5 ms). The floor is then released and the rate climbs back at 2% per frame. A GPU that
    // clocked down while slowed shows a higher cost here, which keeps the floor (as before).
    // LM_PACING_FLOOR_RELEASE=0: the old rule (A/B).
    static const bool sRelease = [] { const char* v = std::getenv("LM_PACING_FLOOR_RELEASE"); return !(v && *v == '0'); }();
    if (sRelease && g.limit > 0 && start >= g.holdUntil && start < g.floorUntil && sCostNoHold.size() >= 15) {
        std::vector<double> v(sCostNoHold.begin(), sCostNoHold.end());
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        if (v[v.size() / 2] < 0.75 * target) {
            g.floorUntil = start;
            log("[pacing] frames cost %.1f ms again (target %.1f ms): the rate climbs back from %.1f fps", v[v.size() / 2] * 1e3,
                target * 1e3, 1.0 / g.limit);
        }
    }
    if (g.limit > 0 && start >= g.holdUntil && misses == 0 && late * 3 < n) {
        g.limit *= 0.98;
        if (start < g.floorUntil) g.limit = std::max(g.limit, g.floor);
        if (g.limit <= target * 1.005) g.limit = 0;
    }
}

void perfReport() {
    static const double sEvery = [] { const char* e = std::getenv("LM_PERF_LOG"); return e ? std::atof(e) : 60.0; }();
    if (sEvery <= 0) return;
    static Clock::time_point sSince = Clock::now();
    static u32 sGuest = frame::guestFrames(), sSlow = 0, sHitch = 0;
    const auto now = Clock::now();
    const double span = secs(now - sSince);
    if (span < sEvery) return;
    Perf& p = sPerf;
    const u32 guest = frame::guestFrames();
    if (p.host.size() >= 30) {
        const auto stat = [](std::vector<float>& v, char* out, size_t n) {
            std::sort(v.begin(), v.end());
            std::snprintf(out, n, "%.1f/%.1f/%.1f", v[v.size() / 2], v[v.size() * 95 / 100], v.back());
        };
        char h[48], w[48], g[48];
        stat(p.host, h, sizeof(h));
        stat(p.worker, w, sizeof(w));
        stat(p.gpu, g, sizeof(g));
        const double fps = p.presents > 1 && p.last > p.first ? double(p.presents - 1) / (p.last - p.first) : 0.0;
        log("[perf] last %.0f s: %.1f fps presented (target %d%s), game %.1f fps, %u intervals over 1.5x; ms per frame "
            "median/p95/max: host %s, render worker %s, GPU %s; slowdowns %u, hitches %u",
            span, fps, frame::targetFps(), sGov.limit > 0 ? ", limited now" : "", double(guest - sGuest) / span, p.over, h, w, g,
            sGov.slowdowns - sSlow, sGov.hitches - sHitch);
    }
    sSince = now;
    sGuest = guest;
    sSlow = sGov.slowdowns;
    sHitch = sGov.hitches;
    p = Perf{};
}

// --- schedule ------------------------------------------------------------------------------------
enum class Mode { Off, Fifo, Vblank, Grid };
Mode sMode = Mode::Off;
Clock::time_point sNext{};       // scheduled start of the next frame (Grid)
Clock::time_point sFrameTime{};  // this frame's time
double sPeriod = 0;              // current frame period (s)
u32 sDivisor = 1;                // Vblank: refreshes per frame
u64 sStartVblank = 0;            // Vblank: refresh count at the previous frame's start
bool sVrrSeen = false;
float sVrrHz = 0;
char sStatus[128] = "";
std::deque<Clock::time_point> sStarts;
bool sPacing = false;

bool detectVrr(float hz) {
    if (hz != sVrrHz) {
        sVrrSeen = false;
        sVrrHz = hz;
    }
    if (sVrrSeen || hz <= 0) return sVrrSeen;
    std::lock_guard lk(sVb.m);
    if (sVb.intervals.size() < 120) return false;
    const double period = 1000.0 / hz;
    u32 odd = 0, n = 0;
    for (double d : sVb.intervals) {
        if (d > 60.0) continue;
        n++;
        const double k = std::max(1.0, std::round(d / period));
        if (std::fabs(d - k * period) > 0.8) odd++;
    }
    // Refreshes that are not multiples of the refresh period: the display follows the presents.
    if (n >= 100 && odd * 10 > n) {
        sVrrSeen = true;
        log("[pacing] variable refresh rate detected (%u of %u refresh intervals off the %.2f Hz grid)", odd, n, hz);
    }
    return sVrrSeen;
}

}  // namespace

bool vrrActive() {
    const int s = port::settings().video.vrr;
    return s == 1 || (s == 0 && sVrrSeen);
}

bool gamescope() {
    // Gamescope sets GAMESCOPE_WAYLAND_DISPLAY for the programs it runs (Steam Deck game mode, or
    // `gamescope -- <program>` on a desktop). LM_GAMESCOPE=0/1 (test aid) overrides the detection.
    static const bool sOn = [] {
        if (const char* e = std::getenv("LM_GAMESCOPE")) return *e == '1';
        const char* d = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
        const char* desk = std::getenv("XDG_CURRENT_DESKTOP");
        return (d && *d) || (desk && std::strstr(desk, "gamescope"));
    }();
    return sOn;
}

bool presentTimingAvailable() {
    // LM_PRESENT_TIMING=0/1 (test aid): off / on on any system.
    static const bool sOn = [] {
        if (const char* e = std::getenv("LM_PRESENT_TIMING")) return *e == '1';
#ifdef _WIN32
        return true;
#else
        return false;
#endif
    }();
    return sOn;
}

bool keepWarmAvailable() { return presentTimingAvailable(); }

bool keepWarmOn() { return keepWarmAvailable() && port::settings().rendering.gpuKeepWarm; }

bool vsyncOn() {
    static const bool sKeep = [] { const char* e = std::getenv("LM_GAMESCOPE_VSYNC"); return e && *e == '0'; }();
    return port::settings().video.vsync || (gamescope() && !sKeep);
}

namespace {
// Fixed refresh: refreshes per frame for a target frame period (at most 4). LM_PACING_DIVISOR=n
// (test aid): at least every n-th refresh.
u32 minDivisor(double minPeriod, double refresh) {
    static const u32 sMinDiv = [] { const char* e = std::getenv("LM_PACING_DIVISOR"); return e ? u32(std::max(1, std::atoi(e))) : 1u; }();
    return std::min<u32>(std::max(u32(std::max(1.0, std::ceil(minPeriod / refresh - 0.02))), sMinDiv), 4);
}
}  // namespace

float fixedRefreshFps(int fps) {
    const float hz = frame::displayRefreshHz();
    if (!vsyncOn() || hz <= 0 || vrrActive() || fps <= 0) return 0.0f;
    return hz / float(minDivisor(1.0 / double(fps), 1.0 / double(hz)));
}

namespace {
// The frame-ahead limit (aurora_wait_frame_done) needs the GPU completion of each frame (present
// timing, Aurora 0197/0216). A backend or driver that does not report it would make every frame
// wait for the full timeout; after a few in a row frames are paced by the timer alone until
// completions are reported again (issue #11: never wait on a signal that does not come).
struct FrameAhead {
    u32 timeouts = 0;    // in a row
    u32 recovered = 0;   // completions seen in a row while off
    bool off = false;
} sAhead;

void waitFrameAhead(u64 frame) {
    FrameAhead& a = sAhead;
    if (a.off) {
        // Probe without waiting; back on after a few frames whose completion arrived in time.
        if (aurora_wait_frame_done(frame, 0)) {
            if (++a.recovered >= 30) {
                a.off = false;
                a.timeouts = 0;
                log("[pacing] GPU frame completions are reported again: frame-ahead limit back on");
            }
        } else {
            a.recovered = 0;
        }
        return;
    }
    if (aurora_wait_frame_done(frame, 50'000'000)) {
        a.timeouts = 0;
        return;
    }
    if (++a.timeouts >= 8) {
        a.off = true;
        a.recovered = 0;
        log("[pacing] %u frames in a row were not reported finished by the GPU within 50 ms (a slow GPU, or present "
            "timing not working: %llu frames never reported): frames are paced by the timer alone, without the "
            "frame-ahead limit",
            a.timeouts, (unsigned long long)aurora_present_timing_timeouts());
    }
}
}  // namespace

void beforeFrame() {
    // The host thread waits for each frame's scheduled start (SDL_DelayPrecise); at normal priority
    // that wait overslept by ~10 ms about once in 5 minutes (the thread not scheduled back in time
    // among the guest, FIFO, render worker, audio and pipeline threads).
    static const bool sPriority = SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    (void)sPriority;
    cpu::report();
    pollTimings();
    perfReport();
    vblankFollowWindow();
    const auto now = Clock::now();
    const Clock::time_point prevFrameTime = sFrameTime;
    sFrameTime = now;
    sPacing = frame::interpolating();
    sCur = FrameRec{};
    sCur.enter = sCur.sched = sCur.woke = sCur.started = now;
    if (!sPacing) {
        sMode = Mode::Off;
        sNext = now;
        sStatus[0] = 0;
        sCur.mode = 'L';
        sCur.target = 1001.0 / 30000.0;
        return;
    }
    sCur.target = 1.0 / double(frame::targetFps());
    const auto& v = port::settings().video;
    const float hz = frame::displayRefreshHz();
    const int fps = frame::targetFps();
    detectVrr(hz);
    if (v.unlimitedFps) {
        // Unlimited: no schedule, no cap and no wait for the GPU; each frame starts as soon as the
        // previous one is handed over (only the swap chain's VSync, if on, still limits it).
        sMode = Mode::Off;
        sNext = now;
        sStarts.push_back(now);
        while (sStarts.size() > 1 && sStarts.back() - sStarts.front() > std::chrono::seconds(1)) sStarts.pop_front();
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps (unlimited%s)", presentedFps(), vsyncOn() ? ", VSync on" : "");
        sCur.mode = 'U';
        sCur.target = 0;
        return;
    }
    const bool vrr = vrrActive();
    // Phone: no governor. The player picks 30 / 60 / 120 and gets exactly that schedule; a frame
    // that runs late is late, the rate is never lowered on its own (user request).
    const bool phone = mobile::enabled();
    const double need = phone ? 0.0 : sGov.limit;  // the period the PC sustains (governor), 0 = the target holds
    const double minPeriod = 1.0 / double(fps);
    Mode mode;
    double period, base;
    const bool vsync = vsyncOn();
    if (phone) {
        // Phone: an even schedule at the target. ProMotion panels follow the presents like a VRR
        // display; counting display-link refreshes (Vblank mode) miscounted there and a 60 fps cap
        // ran at 72-75 fps (build 11 log), while this schedule held 60.0 with no drops. Never
        // faster than the panel (120 fps on a 60 Hz iPhone: 60).
        const int maxHz = mobile::native::maxRefreshRate();
        mode = Mode::Grid;
        base = maxHz > 0 ? std::max(minPeriod, 1.0 / double(maxHz)) : minPeriod;
        period = base;
    } else if (!vsync || hz <= 0) {
        mode = Mode::Grid;
        base = minPeriod;
        period = std::max(minPeriod, need);
    } else if (vrr) {
        // Inside the VRR range: a little below the refresh rate (at the ceiling the display falls
        // back to fixed refresh timing and the swap chain queue adds latency).
        const double cap = 1.0 / std::max(30.0, double(hz) - std::max(3.0, double(hz) * 0.03));
        mode = Mode::Grid;
        base = std::max(minPeriod, cap);
        period = std::max(base, need);
    } else {
        // Fixed refresh: every n-th refresh, never an uneven rate.
        const double refresh = 1.0 / double(hz);
        const u32 n0 = minDivisor(minPeriod, refresh);
        u32 n = u32(std::max(1.0, std::ceil(std::max(minPeriod, need) / refresh - 0.02)));
        n = std::min<u32>(std::max(n, n0), 4);
        sDivisor = n;
        base = double(n0) * refresh;
        period = double(n) * refresh;
        mode = n == 1 ? Mode::Fifo : vblankAvailable() ? Mode::Vblank : Mode::Grid;
    }
    if (mode == Mode::Grid) {
        // The governor already smooths changes (it slows down only on repeated misses and speeds
        // up 2% per frame): follow it.
        sPeriod = period;
        if (sMode != Mode::Grid) sNext = now;
        sNext += dur(sPeriod);
        if (sNext < now - dur(sPeriod * 0.5)) sNext = now;  // fell behind (a load, a hitch): start over
        if (sNext > now) SDL_DelayPrecise(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(sNext - now).count()));
        sFrameTime = sNext;
        sCur.mode = 'G';
    } else if (mode == Mode::Vblank) {
        sPeriod = period;
        std::unique_lock lk(sVb.m);
        if (sMode != Mode::Vblank || sVb.count < sStartVblank || sVb.count > sStartVblank + sDivisor + 2)
            sStartVblank = sVb.count;  // (re)start: from the next refresh
        const u64 target = sStartVblank + sDivisor;
        // At most a period after the previous start: a display that refreshes only when
        // a frame arrives (VRR active although set Off) would otherwise hold every frame until
        // its lowest refresh rate; then the start is the plain grid time.
        // (The condition variable's timeout is only good to a millisecond or two on Windows: the
        // last 2 ms before the grid time are a precise sleep; 59.5 fps instead of 60 before.)
        const auto grid = (sMode == Mode::Vblank ? prevFrameTime : now) + dur(period);
        const auto ready = [&] { return sVb.count >= target; };
        bool hit = sVb.cv.wait_until(lk, grid - dur(0.002), ready);
        if (!hit) {
            lk.unlock();
            const auto left = grid - Clock::now();
            if (left > Clock::duration::zero())
                SDL_DelayPrecise(u64(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count()));
            lk.lock();
            hit = ready();
        }
        sStartVblank = sVb.count;
        sFrameTime = hit ? sVb.last : grid;
        sCur.mode = 'V';
    } else {  // Fifo: the swap chain paces at the refresh rate
        sPeriod = period;
        sFrameTime = now;
        sCur.mode = 'F';
    }
    sCur.sched = sFrameTime;
    sCur.woke = Clock::now();
    sCur.need = need;
    sCur.period = sPeriod;
    const bool sameMode = sMode == mode;
    sMode = mode;
    // The GPU at most one frame behind: the time from a frame's start to its display stays constant.
    const u64 begun = aurora_frame_counter();
    if (mode != Mode::Fifo && begun > 1) waitFrameAhead(begun - 1);
    sStarts.push_back(Clock::now());
    sCur.started = sStarts.back();
    while (sStarts.size() > 1 && sStarts.back() - sStarts.front() > std::chrono::seconds(1)) sStarts.pop_front();
    if (!phone) governFrame(sCur.started, sPeriod, base, sameMode);
    const float shown = presentedFps();
    if (phone) {
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps", shown);
        return;
    }
    char why[96] = "";
    if (need > minPeriod * 1.02) {
        // What limits it: the busiest stage of recent frames (GPU, or the CPU side).
        const double cost = median(sCost), gpu = median(sGpuCost);
        if (gpu > 0 && gpu >= cost * 0.9)
            std::snprintf(why, sizeof(why), " (PC too slow for %d: GPU %.1f ms per frame)", fps, gpu * 1000.0);
        else if (cost > 0)
            std::snprintf(why, sizeof(why), " (PC too slow for %d: CPU %.1f ms per frame)", fps, cost * 1000.0);
        else
            std::snprintf(why, sizeof(why), " (PC too slow for %d)", fps);
    }
    if (mode == Mode::Fifo)
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps (%.0f Hz)", shown, hz);
    else if (mode == Mode::Vblank)
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps = %.0f Hz / %u%s", shown, hz, sDivisor, why);
    else if (vrr && vsync)
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps (variable refresh)%s", shown, why);
    else
        std::snprintf(sStatus, sizeof(sStatus), "%.0f fps%s", shown, why);
}

void afterBegin() { sCur.opened = Clock::now(); }
void afterReplay() { sCur.replayed = Clock::now(); }

namespace {
std::deque<double> sLeads;  // recent frames' time from their scheduled start to beforeEnd (s)
}

void beforeEnd() {
    // Test aid LM_PACING_STALL=<start s>,<ms per frame>,<seconds>: each frame in that window takes
    // that much longer on the host (a transient stall, e.g. another program's load), for governor tests.
    static const auto sStall = [] {
        struct S { double at = 0, ms = 0, secs = 0; } s;
        if (const char* e = std::getenv("LM_PACING_STALL")) std::sscanf(e, "%lf,%lf,%lf", &s.at, &s.ms, &s.secs);
        return s;
    }();
    if (sStall.ms > 0) {
        const double t = secs(Clock::now() - sLogT0);
        if (t >= sStall.at && t < sStall.at + sStall.secs) SDL_DelayPrecise(u64(sStall.ms * 1e6));
    }
    sCur.ending = Clock::now();
    static const bool sOff = [] { const char* e = std::getenv("LM_PRESENT_HOLD"); return e && *e == '0'; }();
    if (sOff || !sPacing || sMode != Mode::Grid) {
        sLeads.clear();
        return;
    }
    const auto now = Clock::now();
    const double lead = secs(now - sFrameTime);
    if (lead < 0 || lead > 4.0 * sPeriod) {  // a hitch (load, compile): start over
        sLeads.clear();
        return;
    }
    // The usual lead: the 90th percentile of the last ~half second; at most 85% of the period (the
    // next frame's start must not wait for this one), and a frame later than that goes at once.
    if (sLeads.size() >= 16) {
        std::vector<double> v(sLeads.begin(), sLeads.end());
        std::nth_element(v.begin(), v.begin() + v.size() * 9 / 10, v.end());
        const double target = std::min(v[v.size() * 9 / 10], 0.85 * sPeriod);
        if (lead < target) {
            SDL_DelayPrecise(u64((target - lead) * 1e9));
            sCur.held = target - lead;
        }
    }
    sLeads.push_back(lead);
    while (sLeads.size() > 60) sLeads.pop_front();
}

void afterFrame() {
    if (!sPacing && !sLog && !keepWarmOn()) return;
    FrameRec r = sCur;
    r.frame = aurora_frame_counter();
    r.begun = sFrameTime;
    r.ended = Clock::now();
    // (begun: the frame's start after any scheduling wait; the CPU part is begin..end)
    if (sPacing && !sStarts.empty()) r.begun = sStarts.back();
    if (!sPacing) r.begun = r.started;
    if (sLog) {
        const interp::PresentInfo pi = interp::lastPresent();
        r.tape = pi.bSeq;
        r.alpha = pi.alpha;
        r.newestTape = pi.newestSeq;
        r.newestDone = pi.newestDone;
        r.stalled = pi.stalled;
        r.warm = sWarm.units;
    }
    sFrames.push_back(r);
    while (sFrames.size() > 16) sFrames.pop_front();
    // Present timing records the GPU completion of every frame (one fence wait per frame on a
    // helper thread, Aurora patch 0197). Not on Linux (presentTimingAvailable).
    static bool enabled = false;
    if (!enabled && presentTimingAvailable()) aurora_present_timing_enable(enabled = true);
}

// (Fifo: the swap chain's wait happens in aurora_begin_frame, after beforeFrame: the time the
// replay starts is the better estimate there.)
Clock::time_point frameTime() { return sPacing && sMode != Mode::Fifo ? sFrameTime : Clock::now(); }
const char* status() { return sStatus; }
float presentedFps() {
    if (sStarts.size() < 2) return 0.0f;
    return float(double(sStarts.size() - 1) / secs(sStarts.back() - sStarts.front()));
}

}  // namespace recomp::pacing
