#include "log.h"
#include <chrono>
#include <filesystem>
#include <print>

namespace fs = std::filesystem;

namespace enu::log {

void print(Level level, Loc loc, std::string_view args) {
  FILE* target;
  std::string_view color;
  switch (level) {
    case Level::info:
      target = stdout;
      break;
    case Level::warn:
      target = stderr;
      color = "\x1b[1;38;5;3m";
      break;
    case Level::error:
    case Level::fatal:
      target = stderr;
      color = "\x1b[1;38;5;1m";
      break;
    default:
      // impossible
      break;
  };

  fs::path path{loc.fpath};
  std::string filename = path.filename();
  std::chrono::time_point time = std::chrono::system_clock::now();

  std::println(
    target,
    "{:%Y-%m-%d %H:%M:%S} [{}:{}] enu::{}{} {} \033[0m",
    time,
    filename,
    loc.line,
    loc.func,
    color,
    args);
}

} // namespace enu::log
