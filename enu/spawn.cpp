#include "spawn.h"

namespace enu {

#ifdef __APPLE__
Pipe::Pipe() {
  int fds[2];

  if (pipe(fds) == -1)
    pfatal("pipe");

  auto fail = [&] {
    int error = errno;
    close(fds[0]);
    close(fds[1]);
    errno = error;
    pfatal("fcntl()");
  };

  for (int fd : fds) {
    int flags;
    if ((flags = fcntl(fd, F_GETFD, 0)) == -1)
      fail();
    if ((flags = fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) == -1)
      fail();
  }
  rd = fds[0];
  wr = fds[1];
}
#else
Pipe::Pipe() {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) == -1)
    pfatal("pipe2");
  read = fds[0];
  write = fds[1];
}
#endif

int Process::wait() {
  int status;
  if (waitpid(pid, &status, 0) == -1)
    return 1;

  // Mark process as dead.
  pid = -1;

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    if (sig == SIGTERM) // We count SIGTERM as a graceful termination.
      return 0;
    return 128 + sig;
  }
  return status;
}

int Process::terminate() {
  int exit_code = 1;
  if (pid == -1)
    return exit_code;

  if (kill(pid, SIGTERM) == 0)
    exit_code = wait();
  for (int fd : {in_wr, out_rd, err_rd})
    if (fd != STDIN_FILENO && fd != STDERR_FILENO && fd != STDOUT_FILENO)
      close(fd);
  return exit_code;
}

} // namespace enu
