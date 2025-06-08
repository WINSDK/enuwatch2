#include "argparser.h"
#include "log.h"
#include "watcher.h"

#include <filesystem>
#include <print>

struct SSH {
  pid_t pid;
  int in_fd;
  int out_fd;

  int wait_on() {
    int status = 0;
    if (waitpid(pid, &status, 0) == -1)
      return -1;

    if (WIFEXITED(status))
      return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
      return 128 + WTERMSIG(status);
    return -1;
  }
};

template<std::convertible_to<const char*>... S>
SSH run_ssh_process(const char* prog, S&&... s) {
  int to[2], from[2];
  if (pipe(to) == -1 || pipe(from) == -1)
    fatal("pipe(..) couldn't spawn necessary ssh instance: {}", strerror(errno));

  pid_t pid = fork();
  if (pid < 0)
    fatal("fork(..) couldn't spawn necessary ssh instance: {}", strerror(errno));

  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd != -1) {
      // dup2(fd, STDERR_FILENO); // silence stderr
      close(fd);
    }

    dup2(to[0], STDIN_FILENO);     // remote stdin
    dup2(from[1], STDOUT_FILENO);  // remote stdout

    close(to[0]);
    close(to[1]);
    close(from[0]);
    close(from[1]);

    execlp(prog, prog, std::forward<S>(s)..., nullptr);
    _exit(127); // cond where exec failed
  }

  close(to[0]);
  close(from[1]);

  return {pid, to[1], from[0]};
}

int main(int argc, const char* argv[]) {
  enu::Args arg = enu::Args::parse(argc, argv);

  if (arg.path.empty()) {
    arg.path = std::filesystem::current_path().string();
  } else {
    if (std::filesystem::is_symlink(arg.path))
      arg.path = std::filesystem::read_symlink(arg.path);

    if (!std::filesystem::is_directory(arg.path))
      fatal("path '{}' is not a valid directory", arg.path);
  }

  SSH ssh = run_ssh_process("ssh", arg.user_host.c_str(), "enu", "--daemon");

  using namespace enu::watcher;
  Watcher watcher(arg.path, arg.latency);
  watcher.run_in_thread([&](FsEvent e) {
    std::string_view k;
    switch (e.kind) {
      case FsEventKind::created:
        k = "created";
        break;
      case FsEventKind::deleted:
        k = "deleted";
        break;
      case FsEventKind::modify:
        k = "modify";
        break;
      case FsEventKind::meta:
        k = "meta";
        break;
      case FsEventKind::moved:
        k = "moved";
        break;
      case FsEventKind::sync:
        k = "sync";
        break;
      case FsEventKind::unrecoverable:
        k = "unrecoverable";
        break;
    }
    std::println("kind: {}, path: '{}', dest: '{}'", k, e.path, e.dest);
  });

  int exit_code = ssh.wait_on();
  if (exit_code != 0)
    fatal("remote instance of enu failed with exit code '{}'", exit_code);
}
