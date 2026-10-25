#include "mobile/session_log.h"

#include "mobile/native.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mobile::sessionlog {
namespace {

constexpr int kKeep = 10;  // sessions kept, this one included

std::atomic<bool> sActive{false};
fs::path sPath, sPrevious;
const auto sStart = std::chrono::steady_clock::now();
#ifndef _WIN32
int sFileFd = -1;     // the log file (O_APPEND: one write per line)
int sPipeRead = -1;   // stdout/stderr arrive here
int sConsoleFd = -1;  // the original stdout (Xcode console / PC terminal): every line is copied there too
std::atomic<unsigned long long> sLinesWritten{0};
#else
std::FILE* sFile = nullptr;
#endif

// "2026-10-25 14:03:22.123 +12.345 " (local wall clock, seconds since the app started).
size_t prefix(char* out, size_t size) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const double up = std::chrono::duration<double>(std::chrono::steady_clock::now() - sStart).count();
    char date[32];
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm);
    const int n = std::snprintf(out, size, "%s.%03d +%.3f ", date, ms, up);
    return n > 0 ? std::min(size_t(n), size - 1) : 0;
}

std::string stampName() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[48];
    std::strftime(buf, sizeof(buf), "session_%Y%m%d_%H%M%S.log", &tm);
    return buf;
}

// Oldest sessions out (names sort by time), so that with the new one kKeep remain.
void prune(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> logs;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("session_", 0) == 0 && e.path().extension() == ".log") logs.push_back(e.path());
    }
    std::sort(logs.begin(), logs.end());
    if (!logs.empty()) sPrevious = logs.back();
    while (int(logs.size()) > kKeep - 1) {
        fs::remove(logs.front(), ec);
        logs.erase(logs.begin());
    }
}

#ifndef _WIN32
void writeAll(int fd, const char* p, size_t n) {
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= size_t(w);
    }
}

void emit(const char* text, size_t len) {
    char head[64];
    const size_t h = prefix(head, sizeof(head));
    // One write per line: appends from the crash path (crashMode) never cut into a line.
    std::string out;
    out.reserve(h + len + 1);
    out.append(head, h).append(text, len).push_back('\n');
    writeAll(sFileFd, out.data(), out.size());
    if (sConsoleFd >= 0) writeAll(sConsoleFd, text, len), writeAll(sConsoleFd, "\n", 1);
    native::mirrorToSystemLog(text, len);
    sLinesWritten.fetch_add(1, std::memory_order_relaxed);
}

void pump() {
#ifdef __APPLE__
    pthread_setname_np("LM session log");
#else
    pthread_setname_np(pthread_self(), "LM session log");
#endif
    std::string pending;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(sPipeRead, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        pending.append(buf, size_t(n));
        size_t start = 0;
        for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1) {
            size_t end = nl;
            if (end > start && pending[end - 1] == '\r') --end;
            emit(pending.data() + start, end - start);
        }
        pending.erase(0, start);
        // A line without its newline yet stays pending, unless it grows absurdly long.
        if (pending.size() > 64 * 1024) {
            emit(pending.data(), pending.size());
            pending.clear();
        }
    }
    if (!pending.empty()) emit(pending.data(), pending.size());
}
#endif

}  // namespace

void start(const fs::path& dir) {
    if (sActive) return;
    const char* force = std::getenv("LM_SESSION_LOG");
    if (!native::isIOS() && !(force && *force == '1')) return;
    std::error_code ec;
    fs::create_directories(dir, ec);
    prune(dir);
    sPath = dir / stampName();
#ifndef _WIN32
    sFileFd = ::open(sPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (sFileFd < 0) return;
    int fds[2];
    if (::pipe(fds) != 0) {
        ::close(sFileFd);
        sFileFd = -1;
        return;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    // On a PC (LM_SESSION_LOG=1) the terminal keeps showing everything. On iOS the original stdout
    // is the debugger console (or nothing), copied too: harmless and useful under Xcode.
    sConsoleFd = ::dup(STDOUT_FILENO);
    sPipeRead = fds[0];
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread(pump).detach();
#else
    sFile = _wfopen(sPath.wstring().c_str(), L"ab");
    if (!sFile) return;
#endif
    sActive = true;
}

bool active() { return sActive; }
fs::path path() { return sPath; }
fs::path previousPath() { return sPrevious; }

void line(const char* category, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::printf("[%s] %s\n", category, msg);
    std::fflush(stdout);
#ifdef _WIN32
    if (sFile) {
        char head[64];
        const size_t h = prefix(head, sizeof(head));
        std::fprintf(sFile, "%.*s[%s] %s\n", int(h), head, category, msg);
        std::fflush(sFile);
    }
#endif
}

void flush() {
    std::fflush(stdout);
    std::fflush(stderr);
#ifndef _WIN32
    if (!sActive) return;
    // Until the pipe is empty and the pump has written what it read (at most half a second).
    for (int i = 0; i < 100; i++) {
        int queued = 0;
        if (::ioctl(sPipeRead, FIONREAD, &queued) != 0 || queued == 0) {
            const auto before = sLinesWritten.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (sLinesWritten.load() == before) return;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
#endif
}

void crashMode() {
#ifndef _WIN32
    if (sFileFd < 0) return;
    ::dup2(sFileFd, STDOUT_FILENO);
    ::dup2(sFileFd, STDERR_FILENO);
#endif
}

}  // namespace mobile::sessionlog
