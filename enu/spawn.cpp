#include "spawn.h"
#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <csignal>
#include <vector>
#include <cstring>
#include <sys/wait.h>

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

#endif

static void close_on_exec(int fd) {
  int flags;
  if ((flags = fcntl(fd, F_GETFD, 0)) == -1)
    pfatal("fcntl(F_GETFD)");
  if ((flags = fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) == -1)
    pfatal("fcntl(F_SETFD)");
}

Pipe::Pipe() {
  int fds[2];
  if (pipe(fds) == -1)
    pfatal("pipe");
  for (int fd : fds)
    close_on_exec(fd);

  rd = fds[0];
  wr = fds[1];
}

struct ChildArgs {
  const char* path;
  char* const* argv;
  Pipe result;
  Pipe in;
  Pipe out;
  Pipe err;
  sigset_t oldmask;
};

static void subprocess(ChildArgs* args) {
  close(args->result.rd);

  auto fail = [&](std::string_view err) {
    const char* errn = strerror(errno);
    size_t errn_len = strlen(errn);

    size_t msg_len = err.size();
    if (errn_len > 0)
      msg_len += strlen(": ") + errn_len;
    write(args->result.wr, &msg_len, sizeof(msg_len));

    write(args->result.wr, err.data(), err.size());
    if (errn_len > 0) {
      write(args->result.wr, ": ", strlen(": "));
      write(args->result.wr, errn, errn_len);
    }
    _exit(127);
  };

  // Block signals, otherwise a signal handler child inherits signal handler from the parent.
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  // Ignore errors as there is no interesting way it can fail.
  for (int sig = 1; sig < NSIG; sig++) {
    // Skip signals that can't be set.
    if (sig == SIGKILL || sig == SIGSTOP)
      continue;
    if (sigaction(sig, &sa, NULL) == -1)
      fail("sigaction (should not happen)");
  }

  int in_fds[3] = {args->in.rd, args->out.wr, args->err.wr};
  int out_fds[3] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};

  /* Use temporary file descriptors for redirections to avoid problems
     when redirecting stdout to stderr for instance. */
  for (size_t idx = 0; idx < 3; idx++) {
    int tmp_fd = dup(in_fds[idx]);
    if (tmp_fd == -1)
      fail("dup");
    close(in_fds[idx]);

    // dup2 clears O_CLOEXEC flag which is necessary here.
    if (dup2(tmp_fd, out_fds[idx]) == -1)
      fail("dup2");
    close(tmp_fd);
  }

  pthread_sigmask(SIG_SETMASK, &args->oldmask, nullptr);

  execve(args->path, args->argv, environ);
  fail("execve");
}

Process _process_spawn(const char* path, char* const* argv) {
  Pipe result{}, in{}, out{}, err{};
  ChildArgs child_args{
    path,
    argv,
    result,
    in,
    out,
    err,
  };

  // Block signals and thread cancellation.
  int cancel_state;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
  sigset_t sigset;
  sigfillset(&sigset);
  pthread_sigmask(SIG_SETMASK, &sigset, &child_args.oldmask);

  pid_t pid = vfork();
  if (pid == 0)
    subprocess(&child_args);
  else if (pid < 0)
    pfatal("vfork()");

  close(result.wr);
  close(in.rd);
  close(out.wr);
  close(err.wr);

  auto restore_signals = [&] {
    pthread_sigmask(SIG_SETMASK, &child_args.oldmask, nullptr);
    pthread_setcancelstate(cancel_state, nullptr);
  };

  /* Blocks until the child closes the pipe.
     If any bytes are read, the child reported an error through this pipe. */
  size_t msg_len;
  if (read(result.rd, &msg_len, sizeof(msg_len)) > 0) {
    std::vector<char> msg(msg_len);
    read(result.rd, msg.data(), msg_len);
    std::string_view msg_s{msg.data(), msg_len};

    int status;
    waitpid(pid, &status, 0);

    close(result.rd);
    close(in.wr);
    close(out.rd);
    close(err.rd);
    restore_signals();

    fatal("path=\"{}\" {}", path, msg_s);
  }

  close(result.rd);
  restore_signals();

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
