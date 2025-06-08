#include "log.h"
#include <chrono>
#include <print>
#include <filesystem>

namespace enu::log {

void print(Level level, Loc loc, std::string_view args) {
  std::string_view color;
  switch (level) {
    case Level::info:
      color = "\x1B[0m";
      break;
    case Level::warn:
      color = "\033[0;32m";
      break;
    case Level::error:
    case Level::fatal:
      color = "\033[0;31m";
      break;
    default:
      // impossible
      break;
  };

  std::filesystem::path path{loc.fpath};
  std::string filename = path.filename();
  std::chrono::time_point time = std::chrono::system_clock::now();

  std::println(
    "{:%Y-%m-%d %H:%M:%S}{} [{}:{}] enu::{} {}\033[0m", time, color, filename, loc.line, loc.func, args);
}

} // namespace enu::log
