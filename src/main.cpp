#include "argparser.h"
#include "log.h"
#include "tracked.h"
#include "watcher.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <limits>
#include <print>
#include <queue>
#include <thread>

using namespace enu;

struct Process {
  pid_t pid;
  int in_fd;
  int out_fd;
  int err_fd;

  int wait_on() {
    int status = 0;

    if (waitpid(pid, &status, 0) == -1)
      return 1;

    if (WIFEXITED(status))
      return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
      return 128 + WTERMSIG(status);
    return status;
  }

  template <typename... Args>
  static Process spawn(const char* prog, Args&&... args) {
    int to[2], from[2], err[2];
    if (pipe(to) == -1 || pipe(from) == -1 || pipe(err) == -1)
      fatal("{}", strerror(errno));

    pid_t pid = fork();
    if (pid < 0)
      fatal("{}", strerror(errno));

    if (pid == 0) {
      // child
      dup2(to[0], STDIN_FILENO);
      dup2(from[1], STDOUT_FILENO);
      dup2(err[1], STDERR_FILENO);

      close(to[0]);
      close(to[1]);
      close(from[0]);
      close(from[1]);
      close(err[0]);
      close(err[1]);

      execlp(prog, prog, std::forward<Args>(args)..., nullptr);
      _exit(errno);
    }

    // parent
    close(to[0]);
    close(from[1]);
    close(err[1]);

    return {pid, to[1], from[0], err[0]};
  }
};

struct [[gnu::packed]] StreamHeader {
  uint8_t version = 1;
};

struct [[gnu::packed]] MessageHeader {
  fs::perms perms;
  fs::file_type ftype;
  uint64_t lens; // 16-bits of path_len, 48-bits of fsize
};

enum class IOKind {
  Ok,
  Fail,
};

struct IOResult {
  IOKind kind = IOKind::Ok;
  size_t n_bytes = 0;

  operator bool() {
    return kind != IOKind::Fail;
  }
};

static IOResult write_partial(int fd, const void* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t b_wrote = write(fd, reinterpret_cast<const uint8_t*>(buf) + off, len - off);
    if (b_wrote == 0)
      break;
    if (b_wrote < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      return {IOKind::Fail, off};
    }
    off += b_wrote;
  }
  return {IOKind::Ok, off};
}

static IOResult write_exact(int fd, const void* buf, size_t len) {
  IOResult r = write_partial(fd, buf, len);
  if (r.n_bytes != len)
    r.kind = IOKind::Fail;
  return r;
}

static IOResult read_partial(int fd, void* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t b_read = read(fd, reinterpret_cast<uint8_t*>(buf) + off, len - off);
    if (b_read == 0)
      break;
    if (b_read < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      return {IOKind::Fail, off};
    }
    off += b_read;
  }
  return {IOKind::Ok, off};
}

static IOResult read_exact(int fd, void* buf, size_t len) {
  IOResult r = read_partial(fd, buf, len);
  if (r.n_bytes != len)
    r.kind = IOKind::Fail;
  return r;
}

const size_t CHUNK_SIZE = 16 * 1024;

static void report_stderr_remote(Process proc) {
  bool header_printed = false;
  auto buf = std::make_unique<uint8_t[]>(CHUNK_SIZE);

  while (true) {
    IOResult r = read_partial(proc.err_fd, buf.get(), CHUNK_SIZE);
    if (r.n_bytes == 0)
      break;
    if (!header_printed) {
      std::print("\x1b[J");
      std::println(stderr, "=================================================================");
      std::println(stderr, "REMOTE ERROR:");
      header_printed = true;
    }
    if (!r) {
      std::println(stderr, "connection closed prematurely (any possible errors lost)");
      break;
    }
    write_exact(STDERR_FILENO, buf.get(), r.n_bytes);
  }

  if (header_printed)
    std::println(stderr, "=================================================================");
}

struct M {
  std::string name;
  fs::perms perms;
  fs::file_type ftype;
  uint64_t fsize = 0;

  explicit M(const fs::path& path) {
    std::error_code ec;
    fs::file_status status = fs::status(path, ec);

    name = path.filename();
    perms = status.permissions();
    ftype = status.type();
    if (ftype == fs::file_type::regular)
      fsize = fs::file_size(path, ec);
  };
};

template <>
struct std::formatter<fs::file_type> : std::formatter<std::string_view> {
  template <class FormatCtx>
  auto format(fs::file_type t, FormatCtx& ctx) const {
    std::string_view name;
    switch (t) {
      case fs::file_type::none:
        name = "none";
        break;
      case fs::file_type::not_found:
        name = "not_found";
        break;
      case fs::file_type::regular:
        name = "regular";
        break;
      case fs::file_type::directory:
        name = "directory";
        break;
      case fs::file_type::symlink:
        name = "symlink";
        break;
      case fs::file_type::block:
        name = "block";
        break;
      case fs::file_type::character:
        name = "character";
        break;
      case fs::file_type::fifo:
        name = "fifo";
        break;
      case fs::file_type::socket:
        name = "socket";
        break;
      case fs::file_type::unknown:
        name = "unknown";
        break;
      default:
        name = "invalid";
        break;
    }
    return std::formatter<std::string_view>::format(name, ctx);
  }
};

constexpr std::string_view SYNC_ON = "\x1b[?2026h";
constexpr std::string_view SYNC_OFF = "\x1b[?2026l";
constexpr std::string_view ERASE_TO_END = "\x1b[J";

static size_t term_height() {
  winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row)
    return ws.ws_row - 1;
  return 23; // Fallback.
}

static std::pair<double, std::string_view> scale(double bytes) {
  const std::array<std::string_view, 5> UNITS = {"B", "KiB", "MiB", "GiB"};

  size_t idx = 0;
  while (bytes >= 1024.0 && idx + 1 < UNITS.size()) {
    bytes /= 1024.0;
    ++idx;
  }
  return {bytes, UNITS[idx]};
}

static size_t display_progress(TrackedNode<M>* root, double elapsed_s) {
  const size_t COL_START = 58;
  const size_t BAR_WIDTH = 40;

  std::string out;
  size_t line_count = 0;
  size_t max_rows = term_height();

  auto total_stats = [&](size_t done, size_t total) {
    double ratio = total ? static_cast<double>(done) / total : 1.0;
    size_t filled = static_cast<size_t>(ratio * BAR_WIDTH);

    auto [done_val, done_unit] = scale(static_cast<double>(done));
    auto [speed_val, speed_unit] = scale(elapsed_s > 0.0 ? done / elapsed_s : 0.0);

    const std::array<std::string_view, 10> X = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    size_t selector = static_cast<size_t>(elapsed_s * 8.0);
    std::string_view sym = X[selector % X.size()];

    return std::format(
      "[{}{}] [{:5.1f}% {:5.1f} {} {:5.1f} {}/s ] {}\n",
      std::string(filled, '='),
      std::string(BAR_WIDTH - filled, ' '),
      std::min(100.0, ratio * 100.0),
      done_val,
      done_unit,
      speed_val,
      speed_unit,
      sym);
  };

  auto node_stats = [](size_t done, size_t total) {
    if (total == 0)
      return std::string("\n");
    const double PCT = total ? 100. * done / total : 100.;
    auto [done_val, done_unit] = scale(static_cast<double>(done));
    if (done < 1024)
      return std::format("[{:5.1f}% {:5.0f}{:3}]\n", PCT, done_val, done_unit);
    else
      return std::format("[{:5.1f}% {:5.1f}{:3}]\n", PCT, done_val, done_unit);
  };

  auto visit = [&](const auto& self, TrackedNode<M>* node, std::string p, size_t p_len, bool last) {
    // stop when we’re out of vertical space
    if (line_count + 1 == max_rows)
      return;

    if (!node->is_root()) {
      out += p;
      out += last ? "└─ " : "├─ ";

      p += last ? "   " : "│  ";
      p_len += 3; // every chunk is 3 cells wide
    }

    std::string_view text = node->meta.name;
    out += text;

    if (p_len + text.size() >= COL_START)
      out += ' ';
    else
      out.append(COL_START - (p_len + text.size()), ' ');

    out += node_stats(node->done(), node->total());
    ++line_count;

    size_t idx = 0;
    node->iter_children([&](TrackedNode<M>* child) {
      ++idx;
      self(self, child, p, p_len, idx == node->child_count());
    });
  };

  visit(visit, root, "", true, 0);

  out += total_stats(root->sum_done(), root->sum_total());
  ++line_count;

  std::print(SYNC_ON);
  std::print(ERASE_TO_END);
  std::print("{}", out);
  // Move cursor to top of frame
  if (line_count > 0)
    std::print("\033[{}A", line_count);
  std::print(SYNC_OFF);
  std::fflush(stdout);

  return line_count;
}

static void clear_progress(TrackedNode<M>* root) {
  // Can't use std::print here because we require async signal safety.
  write(STDOUT_FILENO, SYNC_ON.data(), SYNC_ON.size());
  write(STDOUT_FILENO, ERASE_TO_END.data(), ERASE_TO_END.size());
  write(STDOUT_FILENO, SYNC_OFF.data(), SYNC_OFF.size());
}

static std::string str_to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

static std::jthread scan_dir(const fs::path& root_path, TrackedNode<M>* root_node) {
  return std::jthread{[root_path, root_node] {
    std::vector<std::pair<fs::path, TrackedNode<M>*>> stack;
    stack.emplace_back(root_path, root_node);

    while (!stack.empty()) {
      auto [path, node] = std::move(stack.back());
      stack.pop_back();

      std::error_code ec;
      std::vector<M> entries;
      for (const fs::directory_entry& entry : fs::directory_iterator(path, ec)) {
        if (ec) {
          error("{}", str_to_lower(ec.message()));
          continue;
        }

        entries.emplace_back(M{entry.path()});
      }

      std::sort(entries.begin(), entries.end(), [](const M& a, const M& b) {
        if (a.ftype == fs::file_type::regular && b.ftype == fs::file_type::regular)
          // Sort by smallest size, to ensure we send the smallest files first.
          return a.fsize < b.fsize;
        else
          // Otherwise sort by alphanumeric.
          return a.name < b.name;
      });

      for (M meta : entries) {
        switch (meta.ftype) {
          case fs::file_type::directory: {
            TrackedNode<M>* child = node->add_child(meta);
            fs::path sub_path = fs::path{path / meta.name};

            stack.emplace_back(sub_path, child);
            break;
          }
          case fs::file_type::regular: {
            TrackedNode<M>* child = node->add_child(meta);
            child->shift_total(child->meta.fsize);
            child->unlock();
            break;
          }
          default:
            fs::path sub_path = fs::path{path / meta.name};
            warn("{}: {} file type ignored", sub_path.native(), meta.ftype);
            break;
        }
      }

      node->unlock();
    }
  }};
}

static bool
send_message(int in_fd, const fs::path& base, const fs::path& path, TrackedNode<M>* node) {
  const M& meta = node->meta;

  std::error_code ec;
  fs::path rel_path = fs::relative(path, base, ec);
  if (ec)
    fatal("{}: {}", path.native(), str_to_lower(ec.message()));

  uint64_t rel_path_len = rel_path.native().size();
  if (rel_path_len > std::numeric_limits<std::uint16_t>::max()) {
    warn("path of length {} is too long", rel_path_len);
    return false;
  }

  uint64_t fsize = meta.fsize;
  uint64_t lens = (fsize << 16) | rel_path_len;
  MessageHeader hdr{meta.perms, meta.ftype, lens};

  if (!write_exact(in_fd, &hdr, sizeof(MessageHeader)))
    return false;

  if (!write_exact(in_fd, rel_path.c_str(), rel_path_len))
    return false;

  if (meta.ftype != fs::file_type::regular || fsize == 0)
    return true;

  int out_fd = open(path.c_str(), O_RDONLY);
  if (out_fd == -1)
    fatal("open({}): {}", path.native(), strerror(errno));

  void* map = mmap(nullptr, meta.fsize, PROT_READ, MAP_PRIVATE, out_fd, 0);
  if (map == MAP_FAILED)
    fatal("mmap({}): {}", path.native(), strerror(errno));

  size_t n_sent = 0;
  while (n_sent < fsize) {
    size_t chunk = std::min(CHUNK_SIZE, static_cast<size_t>(fsize) - n_sent);
    IOResult r = write_partial(in_fd, reinterpret_cast<uint8_t*>(map) + n_sent, chunk);
    if (!r || r.n_bytes == 0) {
      close(out_fd);
      return false;
    }
    n_sent += r.n_bytes;
    node->tick(r.n_bytes);
  }

  close(out_fd);
  return true;
};

static bool sync_with_remote(Process proc, const fs::path& root, TrackedNode<M>* root_node) {
  StreamHeader hdr{1};
  if (!write_exact(proc.in_fd, &hdr, sizeof hdr))
    return false;

  fs::path base = root.parent_path();

  std::queue<std::pair<fs::path, TrackedNode<M>*>> q;
  std::unordered_map<TrackedNode<M>*, size_t> pending; // Remaining direct children.

  auto bubble = [&](TrackedNode<M>* node) {
    while (node) {
      auto it = pending.find(node);
      size_t& left = it->second;
      if (--left == 0) {
        TrackedNode<M>* tmp = node->parent;
        node->remove();
        pending.erase(it);
        node = tmp;
      } else {
        break;
      }
    }
  };

  q.emplace(root, root_node);

  while (!q.empty()) {
    auto [path, node] = q.front();
    q.pop();

    if (!send_message(proc.in_fd, base, path, node))
      return false;

    if (node->meta.ftype == fs::file_type::regular) {
      TrackedNode<M>* parent = node->parent;
      node->remove();
      bubble(parent);
    } else {
      size_t count = 0;
      node->iter_children([&](TrackedNode<M>* child) {
        fs::path sub = path / child->meta.name;
        ++count;
        q.emplace(sub, child);
      });
      if (count == 0) { // Empty directory
        TrackedNode<M>* parent = node->parent;
        node->remove();
        bubble(parent);
      } else {
        pending[node] = count;
      }
    }
  }
  return true;
}

static int create_file(std::string_view path, size_t size) {
  int fd = open(path.data(), O_RDWR | O_CREAT);
  if (fd == -1) {
    error("open({}): {}", path, strerror(errno));
    return -1;
  }

  if (ftruncate(fd, size) == -1) {
    error("ftruncate({}): {}", path, strerror(errno));
    close(fd);
    return -1;
  }

  return fd;
}

static bool recv_message(int in_fd) {
  MessageHeader hdr;
  if (!read_exact(in_fd, &hdr, sizeof(MessageHeader)))
    return false;

  uint64_t path_len = hdr.lens & 0xFFFF;
  std::string path(path_len, '\0');
  if (!read_exact(in_fd, path.data(), path_len))
    return false;

  std::error_code ec;
  switch (hdr.ftype) {
    case fs::file_type::regular: {
      uint64_t fsize = hdr.lens >> 16;

      int out_fd = create_file(path, fsize);
      if (out_fd == -1)
        return false;

      if (fsize > 0) { // Important as mmap(..) will fail on mapping 0-sized files.
        void* map = mmap(nullptr, fsize, PROT_WRITE, MAP_SHARED, out_fd, 0);
        if (map == MAP_FAILED)
          fatal("mmap({}): {}", path, strerror(errno));

        if (!read_exact(in_fd, map, fsize)) {
          close(out_fd);
          return false;
        }
      }
      break;
    }
    case fs::file_type::directory: {
      bool created = fs::create_directory(path, ec);
      if (ec) {
        error("{}: {}", path, str_to_lower(ec.message()));
        return false;
      }
      if (!created)
        fatal("directory already exists: {}", path);
      break;
    }
    default:
      break;
  }

  std::filesystem::permissions(path, hdr.perms, std::filesystem::perm_options::add, ec);
  if (ec) {
    warn("{}: {}", path, str_to_lower(ec.message()));
    return true;
  }

  return true;
}

static bool sync_from_remote() {
  int in_fd = STDIN_FILENO;

  StreamHeader hdr;
  if (!read_exact(in_fd, &hdr, sizeof(StreamHeader)))
    return false;
  if (hdr.version != 1)
    fatal("version mismatch");

  while (true) {
    if (!recv_message(in_fd))
      return false;
  }

  return true;
}

static void watch_dir(const Args& arg) {
  using namespace watcher;
  Watcher watcher(arg.path, arg.latency);
  watcher.run_in_thread([](FsEvent e) {
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
    info("notification! kind: {}, path: {}, dest: {}", k, e.path, e.dest);
  });
}

int main(int argc, const char* argv[]) {
  Args arg = Args::parse(argc, argv);

  std::error_code ec;
  if (arg.path.empty()) {
    arg.path = fs::current_path(ec);
    if (ec)
      fatal("{}", str_to_lower(ec.message()));
  } else {
    fs::path path = fs::canonical(arg.path, ec);
    if (ec)
      fatal("{}: {}", arg.path.native(), str_to_lower(ec.message()));
    arg.path = path;

    if (!fs::is_directory(arg.path, ec))
      fatal("{} is not a directory", arg.path.native());
    if (ec)
      fatal("{}", str_to_lower(ec.message()));
  }

  // Ignore "closed pipe" signal.
  signal(SIGPIPE, SIG_IGN);

  if (!arg.daemon) {
    Process proc = Process::spawn(
      "ssh",
      "-o",
      "ServerAliveCountMax=3",
      "-o",
      "ServerAliveInterval=60",
      "-o",
      "ExitOnForwardFailure=yes",
      arg.user_host.c_str(),
      "exec enu --daemon");

    watch_dir(arg);

    {
      static TrackedNode<M> root_node{M{arg.path}, display_progress};
      signal(SIGINT, [](int) { clear_progress(&root_node); });

      std::jthread scan_thread = scan_dir(arg.path, &root_node);

      if (!sync_with_remote(proc, arg.path, &root_node)) {
        error("syncing error");
        clear_progress(&root_node);
        report_stderr_remote(proc);
        return 1;
      }

      signal(SIGINT, SIG_DFL);
      clear_progress(&root_node);
    }

    int exit_code = proc.wait_on();
    if (exit_code != 0) {
      error("something went wrong remotely");
      report_stderr_remote(proc);
      return exit_code;
    }
  } else {
    sync_from_remote();
  }
}
