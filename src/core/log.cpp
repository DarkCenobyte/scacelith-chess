#include "log.h"
#include <cstdio>
#include <mutex>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

namespace logx {
static std::mutex g_mutex;
static FILE* g_file = nullptr;
static auto g_start = std::chrono::steady_clock::now();

void init(const char* path) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (path) g_file = std::fopen(path, "w");
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void vwrite(Level lvl, const char* fmt, va_list ap) {
    static const char* tags[] = {"D", "I", "W", "E"};
    char msg[4096];
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    char line[4200];
    std::snprintf(line, sizeof(line), "[%8.3f %s] %s\n", t, tags[int(lvl)], msg);
    std::lock_guard<std::mutex> lk(g_mutex);
    std::fputs(line, stderr);
    if (g_file) { std::fputs(line, g_file); std::fflush(g_file); }
#ifdef _WIN32
    OutputDebugStringA(line);
#endif
}

void write(Level lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(lvl, fmt, ap);
    va_end(ap);
}
}  // namespace logx
