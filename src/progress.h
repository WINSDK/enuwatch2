#pragma once

#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <print>
#include <string_view>
#include <utility>

namespace enu {

namespace chrono = std::chrono;

inline std::pair<double, std::string_view> scale(double bytes) {
  const std::array<std::string_view, 5> UNITS = {"B", "KiB", "MiB", "GiB"};

  size_t i = 0;
  while (bytes >= 1024.0 && i + 1 < UNITS.size()) {
    bytes /= 1024.0;
    ++i;
  }
  return {bytes, UNITS[i]};
}

class ProgressBar {
  const size_t UPDATE_INTERVAL_MS = 80;

  std::atomic<size_t> total_;
  size_t done_ = 0;
  chrono::steady_clock::time_point start_;
  chrono::steady_clock::time_point last_draw_{start_};

 public:
  explicit ProgressBar(size_t total_bytes = 0)
    : total_(total_bytes), start_(chrono::steady_clock::now()) {}

  // Advances progress and redraws periodically.
  void tick(size_t delta, std::string_view status = "") {
    done_ += delta;

    auto now = chrono::steady_clock::now();
    if (now - last_draw_ < chrono::milliseconds{UPDATE_INTERVAL_MS})
      return;
    last_draw_ = now;

    size_t bar_width = 40;
    double ratio = total_ ? static_cast<double>(done_) / total_ : 1.0;
    size_t filled = static_cast<size_t>(ratio * bar_width);

    auto [done_val, done_unit] = scale(static_cast<double>(done_));
    double elapsed_s = chrono::duration<double>(now - start_).count();
    auto [speed_val, speed_unit] = scale(elapsed_s > 0.0 ? done_ / elapsed_s : 0.0);

    std::print(
      "\r[{}{}] {:6.1f}% {:6.1f} {} {:6.1f} {}/s ",
      std::string(filled, '='),
      std::string(bar_width - filled, ' '),
      std::min(100.0, ratio * 100.0),
      done_val,
      done_unit,
      speed_val,
      speed_unit);

    if (!status.empty())
      std::print("\n{}", status);

    std::fflush(stdout);
  }

  // Increments the total goal atomically.
  void incr_goal(size_t delta) {
    total_ += delta;
  }

  ~ProgressBar() {
    if (done_ > 0) {
      tick(total_ - done_); // Force 100%
      setvbuf(stdout, NULL, _IOFBF, 0);
      std::print("\n");
    }
  }
};

} // namespace enu
