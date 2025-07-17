#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <print>
#include <string>
#include <vector>
#include <enu/spawn.h>

using namespace enu;

bool is_prefix(const char* p, const char* str) {
  return strncmp(p, str, strlen(p)) == 0;
}

void test_process_spawn_basic() {
  std::println("Testing basic Process::spawn()...");

  Process process = Process::spawn("/bin/echo", "hello", "world");
  assert(process.pid > 0);
  assert(process.in_wr >= 0);
  assert(process.out_rd >= 0);
  assert(process.err_rd >= 0);

  int exit_code = process.wait();
  assert(exit_code == 0);

  std::println("Basic Process::spawn() tests passed");
}

void test_process_spawn_with_output() {
  std::println("Testing Process::spawn() with output capture...");

  Process process = Process::spawn("/bin/echo", "test output");

  // Read output.
  char buffer[256];
  ssize_t bytes_read = read(process.out_rd, buffer, sizeof(buffer) - 1);
  assert(bytes_read > 0);
  buffer[bytes_read] = '\0';

  assert(is_prefix("test output", buffer));
  int exit_code = process.wait();
  assert(exit_code == 0);

  std::println("Process::spawn() with output tests passed");
}

void test_process_spawn_with_input() {
  std::println("Testing Process::spawn() with input...");

  Process process = Process::spawn("/bin/cat");

  // Write input.
  const char* input = "hello from stdin\n";
  ssize_t written = write(process.in_wr, input, strlen(input));
  assert(written == strlen(input));

  // Close input to signal EOF.
  close(process.in_wr);

  // Read output.
  char buffer[256];
  ssize_t bytes_read = read(process.out_rd, buffer, sizeof(buffer) - 1);
  assert(bytes_read > 0);
  buffer[bytes_read] = '\0';

  // Should echo back our input.
  assert(is_prefix("hello from stdin", buffer));

  int exit_code = process.wait();
  assert(exit_code == 0);

  std::println("Process::spawn() with input tests passed");
}

void test_process_wait() {
  std::println("Testing Process::wait()...");

  Process process;
  int exit_code;

  process = Process::spawn("/usr/bin/true");
  exit_code = process.wait();
  assert(exit_code == 0);

  process = Process::spawn("/usr/bin/false");
  exit_code = process.wait();
  assert(exit_code == 1);

  process = Process::spawn("/bin/sh", "-c", "exit 42");
  exit_code = process.wait();
  assert(exit_code == 42);

  std::println("Process::wait() tests passed");
}

void test_process_terminate() {
  std::println("Testing Process::terminate()...");

  Process process = Process::spawn("/usr/bin/yes");
  int exit_code = process.terminate();
  assert(exit_code == 0);

  std::println("Process::terminate() tests passed");
}

void test_process_destructor() {
  std::println("Testing Process destructor...");

  pid_t child_pid;
  {
    Process process = Process::spawn("/usr/bin/yes");
    child_pid = process.pid;
  }

  // Process should no longer exist.
  assert(kill(child_pid, 0) == -1 && errno == ESRCH);

  std::println("Process destructor tests passed");
}

void test_signal_handling() {
  std::println("Testing signal handling...");

  int exit_code;
  Process process;

  process = Process::spawn("/usr/bin/yes");
  kill(process.pid, SIGTERM);
  std::println(stderr, "doing wait1");
  exit_code = process.wait();
  assert(exit_code == 0);

  process = Process::spawn("/usr/bin/yes");
  kill(process.pid, SIGKILL);
  exit_code = process.wait();
  assert(exit_code == 128 + SIGKILL);

  std::println("Signal handling tests passed");
}

void test_multiple_processes() {
  std::println("Testing multiple concurrent processes...");

  const size_t NUM_PROCESSES = 10;
  std::vector<Process> processes;
  for (size_t idx = 0; idx < NUM_PROCESSES; ++idx) {
    std::string s_idx = std::to_string(idx);
    processes.emplace_back(Process::spawn("/bin/echo", s_idx.c_str()));
  }

  for (Process& process : processes) {
    int exit_code = process.wait();
    assert(exit_code == 0);
  }

  std::println("Multiple processes tests passed");
}

void test_fd_management() {
  std::println("Testing file descriptor management...");

  // Test that file descriptors are properly managed.
  Process process = Process::spawn("/bin/echo", "fd test");

  assert(process.in_wr >= 0);
  assert(process.out_rd >= 0);
  assert(process.err_rd >= 0);

  assert(process.in_wr != process.out_rd);
  assert(process.in_wr != process.err_rd);
  assert(process.out_rd != process.err_rd);

  int exit_code = process.wait();
  assert(exit_code == 0);

  std::println("File descriptor management tests passed");
}

void test_stdio_redirection() {
  std::println("Testing stdio redirection...");

  // Test that stdin/stdout/stderr are properly redirected.
  Process process = Process::spawn("/bin/sh", "-c", "echo stdout; echo stderr >&2");

  char stdout_buffer[256];
  ssize_t stdout_bytes = read(process.out_rd, stdout_buffer, sizeof(stdout_buffer) - 1);
  assert(stdout_bytes > 0);
  stdout_buffer[stdout_bytes] = '\0';

  char stderr_buffer[256];
  ssize_t stderr_bytes = read(process.err_rd, stderr_buffer, sizeof(stderr_buffer) - 1);
  assert(stderr_bytes > 0);
  stderr_buffer[stderr_bytes] = '\0';

  assert(is_prefix("stdout", stdout_buffer));
  assert(is_prefix("stderr", stderr_buffer));

  int exit_code = process.wait();
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
