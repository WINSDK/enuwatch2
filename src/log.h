#pragma once

#include <format>
#include <cstdint>
#include <string>
#include <unistd.h>

#define ENU_LOC ::enu::log::Loc{__FILE__, __LINE__, __FUNCTION__}

#define info(fmt, ...)                                                                      \
    ::enu::log::print(::enu::log::Level::info, ENU_LOC, std::format(fmt, ##__VA_ARGS__))

#define warn(fmt, ...)                                                                      \
    ::enu::log::print(::enu::log::Level::warn, ENU_LOC, std::format(fmt, ##__VA_ARGS__))

#define error(fmt, ...)                                                                     \
    ::enu::log::print(::enu::log::Level::error, ENU_LOC, std::format(fmt, ##__VA_ARGS__))

#define fatal(fmt, ...)                                                                     \
    (::enu::log::print(::enu::log::Level::fatal, ENU_LOC, std::format(fmt, ##__VA_ARGS__)), \
     ::enu::log::exit_break())

#define pfatal(fmt, ...)                                                                    \
    (::enu::log::print(::enu::log::Level::fatal, ENU_LOC,                                   \
        std::format("{}: {}", std::format(fmt, ##__VA_ARGS__), strerror(errno))),           \
     ::enu::log::exit_break())


namespace enu::log {

[[noreturn]] inline void exit_break() {
#if defined(__x86_64__) || defined(_M_X64)
            __asm__("int3");
#elif defined(__aarch64__) || defined(_M_ARM64)
            __asm__("brk #1");
#endif
  _exit(1);
}


struct Loc {
  std::string_view fpath;
  uint64_t line;
  std::string_view func;
};

enum class Level {
  info,
  warn,
  error,
  fatal,
};

template<typename... Args>
std::string format_args(std::string_view fmt, Args&&... args) {
  return std::format(fmt, std::forward<Args>(args)...);
}

void print(Level level, Loc loc, std::string_view args);

}
