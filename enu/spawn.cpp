#include "log.h"
#include "spawn.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <csignal>
#include <vector>

namespace enu {

#ifdef __APPLE__

// vfork is deprecated on recent version of macOS.
#ifdef __MAC_OS_X_VERSION_MAX_ALLOWED
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
#define vfork fork
#endif
#endif

// `environ` is not defined in any headers on macOS.
extern "C" char** environ;

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

struct ChildArgs {
  const char* path;
  char* const* argv;
  int result_wr;
  int in_rd;
  int out_wr;
  int err_wr;
  sigset_t oldmask;
};

static void subprocess(ChildArgs* args) {
  /* All signal dispositions must be either SIG_DFL or SIG_IGN
   * before signals are unblocked. Otherwise a signal handler
   * from the parent might get run in the child while sharing
   * memory, with unpredictable and dangerous results. */
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  // Ignore errors as there is no interesting way it can fail.
  for (int idx = 1; idx < NSIG; idx++)
    sigaction(idx, &sa, NULL);

  auto fail = [&](std::string_view err) {
    const char* errn = strerror(errno);
    size_t errn_len = strlen(errn);

    size_t msg_len = err.size();
    if (errn_len > 0)
      msg_len += strlen(": ") + errn_len;
    write(args->result_wr, &msg_len, sizeof(msg_len));

    write(args->result_wr, err.data(), err.size());
    if (errn_len > 0) {
      write(args->result_wr, ": ", strlen(": "));
      write(args->result_wr, errn, errn_len);
    }
  };

  int tmp_fds[3];
  int in_fds[3] = {args->in_rd, args->out_wr, args->err_wr};
  int out_fds[3] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};

  /* Use temporary file descriptors for redirections to avoid problems
     when redirecting stdout to stderr for instance. */
  for (size_t idx = 0; idx < 3; idx++) {
    tmp_fds[idx] = dup(in_fds[idx]);
    close(in_fds[idx]);

    if (dup2(tmp_fds[idx], out_fds[idx]) == -1) {
      fail("dup2");
      _exit(127);
    }
    close(tmp_fds[idx]);
  }

  pthread_sigmask(SIG_SETMASK, &args->oldmask, nullptr);

  execve(args->path, args->argv, environ);
  fail("execve");
  _exit(127);
}

Process _process_spawn(const char* path, char* const* argv) {
  // Create a communication pipe for error handling.
  Pipe result{};

  Pipe in{};
  Pipe out{};
  Pipe err{};

  ChildArgs child_args{
    .path = path,
    .argv = argv,
    .result_wr = result.wr,
    .in_rd = in.rd,
    .out_wr = out.wr,
    .err_wr = err.wr,
  };

  // Block signals and thread cancellation.
  int cancel_state;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
  sigset_t sigset;
  sigfillset(&sigset);
  pthread_sigmask(SIG_SETMASK, &sigset, &child_args.oldmask);

  pid_t pid = vfork();

  if (pid == 0) {
    close(result.rd);
    subprocess(&child_args);
  }

  if (pid < 0)
    pfatal("vfork()");

  close(result.wr);

  auto restore_signals = [&] {
    pthread_sigmask(SIG_SETMASK, &child_args.oldmask, nullptr);
    pthread_setcancelstate(cancel_state, nullptr);
  };

  /* Blocks until the child closes the pipe.
     If any bytes are read, the child reported an error through this pipe. */
  size_t msg_len = 0;
  if (read(result.rd, &msg_len, sizeof(msg_len)) > 0) {
    std::vector<char> msg(msg_len + 1);
    read(result.rd, msg.data(), msg_len);
    msg[msg_len] = '\0';

    int status;
    waitpid(pid, &status, 0);

    close(result.rd);
    restore_signals();

    fatal("cmd=\"{}\" {}", path, msg.data());
  }

  close(result.rd);
  restore_signals();

  close(in.rd);
  close(out.wr);
  close(err.wr);

  Process proc;
  proc.pid = pid;
  proc.in_wr = in.wr;
  proc.out_rd = out.rd;
  proc.err_rd = err.rd;
  return proc;
}

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
