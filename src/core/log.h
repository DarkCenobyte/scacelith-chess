// Logging. Never use std::cout / std::cin anywhere in the game: those streams are owned by the
// embedded Stockfish engine (see ai/). Log lines go to stderr, to OutputDebugString on Windows
// and to scacelith.log next to the executable (in the user data directory when the executable's
// folder is read-only, see main.cpp).
#pragma once
#include <cstdarg>

namespace logx {
enum class Level { Debug, Info, Warn, Error };
bool init(const char* logFilePath);  // nullptr = no file; false when no log file is open
void shutdown();
void write(Level lvl, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void vwrite(Level lvl, const char* fmt, va_list ap);
}  // namespace logx

#define LOGD(...) ::logx::write(::logx::Level::Debug, __VA_ARGS__)
#define LOGI(...) ::logx::write(::logx::Level::Info, __VA_ARGS__)
#define LOGW(...) ::logx::write(::logx::Level::Warn, __VA_ARGS__)
#define LOGE(...) ::logx::write(::logx::Level::Error, __VA_ARGS__)
