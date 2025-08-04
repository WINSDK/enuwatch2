#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <csignal>
#include <print>
#include <string>
#include <vector>
#include <enu/spawn.h>
#include <enu/net.h>

using namespace enu;

bool is_prefix(const char* p, const char* str) {
  return strncmp(p, str, strlen(p)) == 0;
}

void test_process_spawn_basic() {
  std::println("Testing basic Process::spawn()...");

  Process proc = Process::spawn("/bin/echo", "hello", "world");
  assert(proc.pid > 0);
  assert(proc.in_wr >= 0);
  assert(proc.out_rd >= 0);
  assert(proc.err_rd >= 0);

  int exit_code = proc.wait();
  assert(exit_code == 0);

  std::println("Basic Process::spawn() tests passed");
}

void test_process_spawn_with_output() {
  std::println("Testing Process::spawn() with output capture...");

  Process proc = Process::spawn("/bin/echo", "test output");

  // Read output.
  char buffer[256];
  IOResult io_r = read_exact(proc.out_rd, buffer, sizeof(buffer) - 1);
  assert(io_r.n_bytes > 0);
  buffer[io_r.n_bytes] = '\0';

  assert(is_prefix("test output", buffer));
  int exit_code = proc.wait();
  assert(exit_code == 0);

  std::println("Process::spawn() with output tests passed");
}

void test_process_spawn_with_input() {
  std::println("Testing Process::spawn() with input...");

  Process proc = Process::spawn("/bin/cat");

  // Write input.
  IOResult io_r;
  const char* input = "hello from stdin\n";
  io_r = write_exact(proc.in_wr, input, strlen(input));
  assert(io_r.n_bytes == strlen(input));

  // Close input to signal EOF.
  close(proc.in_wr);

  // Read output.
  char buffer[256];
  io_r = read_exact(proc.out_rd, buffer, sizeof(buffer) - 1);
  assert(io_r.n_bytes > 0);
  buffer[io_r.n_bytes] = '\0';

  // Should echo back our input.
  assert(is_prefix("hello from stdin", buffer));

  int exit_code = proc.wait();
  assert(exit_code == 0);

  std::println("Process::spawn() with input tests passed");
}

void test_process_wait() {
  std::println("Testing Process::wait()...");

  Process proc;
  int exit_code;

  proc = Process::spawn("/usr/bin/true");
  exit_code = proc.wait();
  assert(exit_code == 0);

  proc = Process::spawn("/usr/bin/false");
  exit_code = proc.wait();
  assert(exit_code == 1);

  proc = Process::spawn("/bin/sh", "-c", "exit 42");
  exit_code = proc.wait();
  assert(exit_code == 42);

  std::println("Process::wait() tests passed");
}

void test_process_terminate() {
  std::println("Testing Process::terminate()...");

  Process proc = Process::spawn("/usr/bin/yes");
  int exit_code = proc.terminate();
  assert(exit_code == 0);

  std::println("Process::terminate() tests passed");
}

void test_process_destructor() {
  std::println("Testing Process destructor...");

  pid_t child_pid;
  {
    Process proc = Process::spawn("/usr/bin/yes");
    child_pid = proc.pid;
  }

  // Process should no longer exist.
  assert(kill(child_pid, 0) == -1 && errno == ESRCH);

  std::println("Process destructor tests passed");
}

void test_signal_handling() {
  std::println("Testing signal handling...");

  int exit_code;
  Process proc;

  proc = Process::spawn("/usr/bin/yes");
  kill(proc.pid, SIGTERM);
  exit_code = proc.wait();
  assert(exit_code == 0);

  proc = Process::spawn("/usr/bin/yes");
  kill(proc.pid, SIGKILL);
  exit_code = proc.wait();
  assert(exit_code == 128 + SIGKILL);

  std::println("Signal handling tests passed");
}

void test_multiple_processes() {
  std::println("Testing multiple concurrent processes...");

  const size_t NUM_PROCESSES = 10;
  std::vector<Process> procs;
  for (size_t idx = 0; idx < NUM_PROCESSES; ++idx) {
    std::string s_idx = std::to_string(idx);
    procs.emplace_back(Process::spawn("/bin/echo", s_idx.c_str()));
  }

  for (Process& proc : procs) {
    int exit_code = proc.wait();
    assert(exit_code == 0);
  }

  std::println("Multiple processes tests passed");
}

void test_fd_management() {
  std::println("Testing file descriptor management...");

  // Test that file descriptors are properly managed.
  Process proc = Process::spawn("/bin/echo", "fd test");

  assert(proc.in_wr >= 0);
  assert(proc.out_rd >= 0);
  assert(proc.err_rd >= 0);

  assert(proc.in_wr != proc.out_rd);
  assert(proc.in_wr != proc.err_rd);
  assert(proc.out_rd != proc.err_rd);

  int exit_code = proc.wait();
  assert(exit_code == 0);

  std::println("File descriptor management tests passed");
}

void test_stdio_redirection() {
  std::println("Testing stdio redirection...");

  // Test that stdin/stdout/stderr are properly redirected.
  Process proc = Process::spawn("/bin/sh", "-c", "echo stdout; echo stderr >&2");

  IOResult io_r;
  char stdout_buffer[256];
  io_r = read_exact(proc.out_rd, stdout_buffer, sizeof(stdout_buffer) - 1);
  assert(io_r.n_bytes > 0);
  stdout_buffer[io_r.n_bytes] = '\0';

  char stderr_buffer[256];
  io_r = read_exact(proc.err_rd, stderr_buffer, sizeof(stderr_buffer) - 1);
  assert(io_r.n_bytes > 0);
  stderr_buffer[io_r.n_bytes] = '\0';

  assert(is_prefix("stdout", stdout_buffer));
  assert(is_prefix("stderr", stderr_buffer));

  int exit_code = proc.wait();
  assert(exit_code == 0);

  std::println("stdio redirection tests passed");
}

int main() {
  std::println("Running spawn tests...");

  test_process_spawn_basic();
  test_process_spawn_with_output();
  test_process_spawn_with_input();
  test_process_wait();
  test_process_terminate();
  test_process_destructor();
  test_signal_handling();
  test_multiple_processes();
  test_fd_management();
  test_stdio_redirection();

  std::println("\nAll spawn tests passed!");
}
