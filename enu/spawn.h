#pragma once

#include <utility>
#include <sys/types.h>

namespace enu {

struct Pipe {
  int rd;
  int wr;

  explicit Pipe();
};

struct Process;
Process _process_spawn(const char* path, char* const* argv);

struct Process {
  pid_t pid;
  int in_wr;
  int out_rd;
  int err_rd;

  template <typename... Args>
  static Process spawn(const char* path, Args&&... args) {
    const char* argv[] = {path, std::forward<Args>(args)..., nullptr};
    return _process_spawn(path, const_cast<char* const*>(argv));
  }

  explicit Process() noexcept : pid(-1) {};

  Process(const Process&) noexcept = delete;
  Process operator=(const Process&) noexcept = delete;

  Process(Process&& src) noexcept {
    move(src);
  }

  Process& operator=(Process&& src) noexcept {
    if (this != &src) {
      terminate();
      move(src);
    }
    return *this;
  }

  ~Process() noexcept {
    terminate();
  }

  int wait();
  int terminate();

 private:
  void move(Process& src) {
    pid = std::exchange(src.pid, -1);
    in_wr = src.in_wr;
    out_rd = src.out_rd;
    err_rd = src.err_rd;
  }
};

} // namespace enu
