// #include "argparser.h"
// #include "log.h"
// #include "progress.h"
// #include "watcher.h"
//
// #include <fcntl.h>
// #include <poll.h>
// #include <sys/mman.h>
// #include <sys/stat.h>
// #include <sys/wait.h>
// #include <cassert>
// #include <cstring>
// #include <filesystem>
// #include <limits>
// #include <print>
// #include <thread>
//
// using namespace enu;
//
// namespace fs = std::filesystem;
//
// struct SSH {
//   pid_t pid;
//   int in_fd;
//   int out_fd;
//   int err_fd;
//
//   int wait_on() {
//     int status = 0;
//     if (waitpid(pid, &status, 0) == -1)
//       return 1;
//
//     if (WIFEXITED(status))
//       return WEXITSTATUS(status);
//     if (WIFSIGNALED(status))
//       return 128 + WTERMSIG(status);
//     return status;
//   }
// };
//
// static pid_t child_pid = 0;
//
// static sigset_t make_block_all() {
//   sigset_t set;
//   sigemptyset(&set);
//   for (int i = 1; i < NSIG; ++i)
//     if (i != SIGKILL && i != SIGSTOP)
//       sigaddset(&set, i);
//   return set;
// }
//
// static void reap_child(int sig) {
//   if (child_pid > 0) // global or captured
//     kill(child_pid, SIGKILL); // async-signal-safe
//   _exit(128 + sig); // mimic default exit code
// }
//
// void install_catch_all(pid_t child) {
//   child_pid = child;
//
//   struct sigaction sa{};
//   sa.sa_handler = reap_child;
//   sa.sa_mask = make_block_all(); // block everything else while in handler
//   sa.sa_flags = SA_RESTART;
//
//   for (int i = 1; i < NSIG; ++i)
//     if (i != SIGKILL && i != SIGSTOP)
//       sigaction(i, &sa, nullptr);
// }
//
// void remember_children(int) {
// 	int status;
// 	pid_t pid;
// 	/* An empty waitpid() loop was put here by Tridge and we could never
// 	 * get him to explain why he put it in, so rather than taking it
// 	 * out we're instead saving the child exit statuses for later use.
// 	 * The waitpid() loop presumably eliminates all possibility of leaving
// 	 * zombie children, maybe that's why he did it. */
// 	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
//     if (child_pid == 0) {
//       child_pid = pid;
//       break;
//     }
// 	}
// }
//
// static struct sigaction sigact;
//
// template <typename... Args>
// static SSH run_ssh_process(const char* prog, Args&&... args) {
//   int to[2], from[2], err[2];
//   if (pipe(to) == -1 || pipe(from) == -1 || pipe(err) == -1)
//     fatal("{}", strerror(errno));
//
//   pid_t pid = fork();
//   if (pid < 0)
//     fatal("{}", strerror(errno));
//
//   install_catch_all(pid);
//
// 	sigset_t sigmask;
// 	sigemptyset(&sigmask);
// 	sigact.sa_flags = SA_NOCLDSTOP;
//   signal(SIGCHLD, remember_children);
//
//
//   if (pid == 0) {
//     // child
//     dup2(to[0], STDIN_FILENO); // stdin
//     dup2(from[1], STDOUT_FILENO); // stdout
//     dup2(err[1], STDERR_FILENO); // stderr
//
//     close(to[0]);
//     close(to[1]);
//     close(from[0]);
//     close(from[1]);
//     close(err[0]);
//     close(err[1]);
//
//     execlp(prog, prog, std::forward<Args>(args)..., nullptr);
//     _exit(errno); // exec failed
//   }
//
//   // parent
//   close(to[0]);
//   close(from[1]);
//   close(err[1]);
//
//   return {pid, to[1], from[0], err[0]};
// }
//
// enum class EntryKind : uint8_t { File, Directory };
//
// struct [[gnu::packed]] Header {
//   uint8_t version = 1;
// };
//
// struct [[gnu::packed]] Message {
//   EntryKind kind;
//   uint64_t path_len : 16;
//   uint64_t data_len : 48;
//   uint8_t data[];
// };
//
// enum class IOKind {
//   Ok,
//   Fail,
//   Done,
// };
//
// struct IOResult {
//   IOKind kind = IOKind::Ok;
//   size_t n_bytes = 0;
//
//   operator bool() {
//     return kind != IOKind::Fail;
//   }
// };
//
// static IOResult write_partial(int fd, uint8_t buf[], size_t len) {
//   size_t off = 0;
//   while (true) {
//     ssize_t b_wrote = write(fd, &buf[off], len - off);
//     if (b_wrote == 0)
//       return {IOKind::Done, off};
//     if (b_wrote < 0) {
//       if (errno == EINTR)
//         continue;
//       return {IOKind::Fail, off};
//     }
//     off += b_wrote;
//   }
// }
//
// static IOResult write_exact(int fd, uint8_t buf[], size_t len) {
//   IOResult r = write_partial(fd, buf, len);
//   if (r.n_bytes != len)
//     r.kind = IOKind::Fail;
//   return r;
// }
//
// static IOResult read_partial(int fd, uint8_t buf[], size_t len) {
//   size_t off = 0;
//   while (true) {
//     ssize_t b_read = read(fd, &buf[off], len - off);
//     if (b_read == 0)
//       return {IOKind::Done, off};
//     if (b_read < 0) {
//       if (errno == EINTR)
//         continue;
//       return {IOKind::Fail, off};
//     }
//     off += b_read;
//   }
// }
//
// static IOResult read_exact(int fd, uint8_t buf[], size_t len) {
//   IOResult r = read_partial(fd, buf, len);
//   if (r.n_bytes != len)
//     r.kind = IOKind::Fail;
//   return r;
// }
//
// const size_t CHUNK_SIZE = 4 * 1024;
//
// void report_stderr_remote(SSH conn) {
//   bool header_printed = false;
//   auto buf = std::make_unique<uint8_t[]>(CHUNK_SIZE);
//
//   while (true) {
//     IOResult r = read_partial(conn.err_fd, buf.get(), CHUNK_SIZE);
//     if (!r || r.n_bytes == 0)
//       break;
//     if (!header_printed) {
//       std::println(stderr, "=================================================================");
//       std::println(stderr, "REMOTE ERROR:");
//       header_printed = true;
//     }
//     write_exact(STDERR_FILENO, buf.get(), r.n_bytes);
//   }
//
//   if (header_printed)
//     std::println(stderr, "=================================================================");
// }
//
// static std::jthread scan_dir_size(std::string_view root, ProgressBar& pg) {
//   auto sum_filesize = [&, root] { // Capturing root is necessary here.
//     for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
//       if (entry.is_regular_file())
//         pg.incr_goal(entry.file_size());
//   };
//
//   return std::jthread{sum_filesize};
// }
//
// static bool sync_to_remote(SSH conn, std::string_view root, ProgressBar& pg) {
//   auto buf = std::make_unique<uint8_t[]>(CHUNK_SIZE);
//
//   std::error_code ec;
//   for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec)) {
//     if (ec) {
//       warn("couldn't read file: {}", ec.message());
//       continue;
//     }
//
//     std::string path = entry.path().string();
//
//     EntryKind kind;
//     uint64_t data_len = 0;
//     fs::file_status status = entry.status();
//     if (fs::is_regular_file(status)) {
//       kind = EntryKind::File;
//       data_len = entry.file_size();
//     } else if (fs::is_directory(status)) {
//       kind = EntryKind::Directory;
//     } else {
//       warn("ignoring unknown file kind: {}", path);
//       continue;
//     }
//
//     if (!write_exact(conn.in_fd, reinterpret_cast<uint8_t*>(&kind), sizeof(kind)))
//       return false;
//
//     uint64_t path_len = path.size();
//     if (path_len > std::numeric_limits<uint16_t>::max())
//       fatal("path of length {} is too long", data_len);
//
//     uint64_t lens = (data_len << 16) | (path_len & 0xFFFF);
//
//     if (!write_exact(conn.in_fd, reinterpret_cast<uint8_t*>(&lens), sizeof(lens)))
//       return false;
//
//     if (!write_exact(conn.in_fd, reinterpret_cast<uint8_t*>(path.data()), path_len))
//       return false;
//
//     if (kind == EntryKind::File) {
//       int fd = open(path.c_str(), O_RDONLY);
//       if (fd == -1) {
//         warn("open({}): {}", path, strerror(errno));
//         continue;
//       }
//
//       size_t n_read = 0;
//       while (n_read < data_len) {
//         IOResult r = read_partial(fd, buf.get(), CHUNK_SIZE);
//         if (!r || !write_exact(conn.in_fd, buf.get(), r.n_bytes)) {
//           close(fd);
//           return false;
//         }
//         n_read += r.n_bytes;
//         pg.tick(CHUNK_SIZE);
//       }
//
//       close(fd);
//     }
//   }
//
//   return true;
// }
//
// static bool sync_initial(SSH conn) {
//   uint8_t version = 1;
//   if (!write_exact(conn.in_fd, &version, sizeof(version)))
//     return false;
//
//   return true;
// }
//
// static bool receiver_loop() {
//   uint8_t version;
//   if (!read_exact(STDIN_FILENO, &version, sizeof(version)))
//     return false;
//   assert(version == 1);
//   std::println(stderr, "established v{}", version);
//
//   auto buf = std::make_unique<uint8_t[]>(CHUNK_SIZE);
//
//   while (true) {
//     EntryKind kind;
//     if (!read_exact(STDIN_FILENO, reinterpret_cast<uint8_t*>(&kind), sizeof(kind)))
//       return false;
//
//     uint64_t lens;
//     if (!read_exact(STDIN_FILENO, reinterpret_cast<uint8_t*>(&lens), sizeof(lens)))
//       return false;
//     uint64_t path_len = lens & 0xFFFF;
//     uint64_t data_len = lens >> 16;
//
//     std::string path(path_len, '\0');
//     if (!read_exact(STDIN_FILENO, reinterpret_cast<uint8_t*>(path.data()), path_len))
//       return false;
//
//     if (kind == EntryKind::File) {
//       size_t n_read = 0;
//       while (n_read < data_len) {
//         IOResult r = read_partial(STDIN_FILENO, buf.get(), CHUNK_SIZE);
//         if (!r)
//           return false;
//         n_read += r.n_bytes;
//       }
//     }
//   }
//
//   return true;
// }
//
// static void watch_dir(const Args& arg) {
//   using namespace watcher;
//   Watcher watcher(arg.path, arg.latency);
//   watcher.run_in_thread([](FsEvent e) {
//     std::string_view k;
//     switch (e.kind) {
//       case FsEventKind::created:
//         k = "created";
//         break;
//       case FsEventKind::deleted:
//         k = "deleted";
//         break;
//       case FsEventKind::modify:
//         k = "modify";
//         break;
//       case FsEventKind::meta:
//         k = "meta";
//         break;
//       case FsEventKind::moved:
//         k = "moved";
//         break;
//       case FsEventKind::sync:
//         k = "sync";
//         break;
//       case FsEventKind::unrecoverable:
//         k = "unrecoverable";
//         break;
//     }
//     std::println("notification! kind: {}, path: {}, dest: {}", k, e.path, e.dest);
//   });
// }
//
// int main(int argc, const char* argv[]) {
//   Args arg = Args::parse(argc, argv);
//
//   if (arg.path.empty()) {
//     arg.path = fs::current_path().string();
//   } else {
//     arg.path = fs::absolute(arg.path);
//
//     if (!fs::is_directory(arg.path))
//       fatal("path {} is not a valid directory", arg.path);
//   }
//
//   // Ignore "closed on pipe" signal.
//   signal(SIGPIPE, SIG_IGN);
//
//   if (!arg.daemon) {
//     SSH conn = run_ssh_process(
//       "ssh",
//       "-t",
//       "-o",
//       "ServerAliveCountMax=3",
//       "-o",
//       "ServerAliveInterval=60",
//       "-o",
//       "ExitOnForwardFailure=yes",
//       arg.user_host.c_str(),
//       "exec enu --daemon");
//
//     watch_dir(arg);
//
//     {
//       // Start scanning dir to figure out the total number of bytes we have to end up
//       transferring. ProgressBar pg; std::jthread scan_t = scan_dir_size(arg.path, pg);
//
//       if (!sync_initial(conn)) {
//         error("handshake error");
//         report_stderr_remote(conn);
//         return 1;
//       }
//
//       if (!sync_to_remote(conn, arg.path, pg)) {
//         error("syncing error");
//         report_stderr_remote(conn);
//         return 1;
//       }
//
//       scan_t.join();
//     }
//
//     int exit_code = conn.wait_on();
//     if (exit_code != 0) {
//       error("something went wrong remotely");
//       report_stderr_remote(conn);
//       return exit_code;
//     }
//   } else {
//     receiver_loop();
//   }
// }

#include <chrono>
#include <string>
#include <thread>
#include "tree_progress.h"

using namespace enu;

int main() {
  ProgressNode root{"Build Project"};

  ProgressNodeRef compile = root.add_child("Compile modules", 5);
  ProgressNodeRef link = root.add_child("Link", 10);
  ProgressNodeRef tests = root.add_child("Run tests", 20);

  ProgressNodeRef t2 = tests->add_child("LOL", 10);

  for (size_t i = 1; i <= 5; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    compile->tick(1);
  }
  compile->finish();

  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  link->tick(10);
  link->finish();

  for (size_t i = 1; i <= 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    tests->tick(1);
  }
  tests->finish();

  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  root.finish();
  // paint.join();
  return 0;
}
