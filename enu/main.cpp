#include "argparser.h"
#include "daemon.h"
#include "log.h"
#include "net.h"
#include "spawn.h"
#include "tracked.h"
#include "watcher.h"

#include <sys/ioctl.h>
#include <tbb/concurrent_hash_map.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/task_group.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <limits>
#include <memory>
#include <print>
#include <thread>

using namespace enu;

const size_t CHUNK_SIZE = 16 * 1024;

static void report_stderr_remote(const Process& proc) {
  bool header_printed = false;
  auto buf = std::make_unique<uint8_t[]>(CHUNK_SIZE);

  while (true) {
    IOResult r = read_partial(proc.err_rd, buf.get(), CHUNK_SIZE);
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

    return std::format("[{}{}] [{:5.1f}% {:5.1f} {} {:5.1f} {}/s ] {}\n",
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

  auto visit = [&](const auto& f, TrackedNode<M>* node, std::string p, size_t p_len, bool last) {
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
      f(f, child, p, p_len, idx == node->child_count());
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

static bool send_datamsg(int sock,
                         uint32_t seq,
                         const fs::path& base,
                         const fs::path& path,
                         TrackedNode<M>* node,
                         const std::atomic<bool>& ok) {
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

  DataMessage hdr{seq, meta.perms, meta.ftype, static_cast<uint16_t>(rel_path_len), meta.fsize};
  if (!write_exact(sock, &hdr, sizeof(hdr)))
    return false;

  if (!write_exact(sock, rel_path.c_str(), rel_path_len))
    return false;

  auto write_file_contents = [&] {
    int out_fd = open(path.c_str(), O_RDONLY);
    if (out_fd == -1)
      fatal("open({})", path.native());

    void* map = mmap(nullptr, meta.fsize, PROT_READ, MAP_PRIVATE, out_fd, 0);
    if (map == MAP_FAILED)
      fatal("mmap({})", path.native());

    size_t n_sent = 0;
    while (n_sent < meta.fsize) {
      size_t chunk = std::min(CHUNK_SIZE, static_cast<size_t>(meta.fsize) - n_sent);
      IOResult r = write_partial(sock, reinterpret_cast<uint8_t*>(map) + n_sent, chunk);
      if (!r || r.n_bytes == 0 || !ok) {
        close(out_fd);
        return false;
      }
      n_sent += r.n_bytes;
      node->tick(r.n_bytes);
    }

    close(out_fd);
    return true;
  };

  if (meta.ftype == fs::file_type::regular && meta.fsize > 0)
    if (!write_file_contents())
      return false;

  return true;
};

static bool sync_with_remote(const fs::path& root, TrackedNode<M>* root_node) {
  fs::path base = root.parent_path();

  using HashMap = tbb::concurrent_hash_map<TrackedNode<M>*, size_t>;
  HashMap pending; // Remaining direct children.

  auto bubble = [&](TrackedNode<M>* node) {
    while (node) {
      HashMap::accessor it;
      pending.find(it, node);

      if (--it->second == 0) {
        TrackedNode<M>* parent = node->parent;
        if (parent)
          node->remove();
        pending.erase(it);
        node = parent;
      } else {
        break;
      }
    }
  };

  Process daemon = Process::spawn("/usr/bin/ssh",
                                  "-o",
                                  "ExitOnForwardFailure=yes",
                                  "-o",
                                  "ControlMaster=auto",
                                  "-o",
                                  "ControlPath=~/.ssh/%h",
                                  "nicolas@localhost",
                                  "exec /Users/nicolas/Projects/enuwatch/build/enu/enu --daemon");

  std::atomic<bool> ok = true;
  auto report_async_error = [&]() {
    if (ok.exchange(false))
      report_stderr_remote(daemon);
  };

  std::atomic<uint32_t> g_seq = 0;
  std::mutex m_master_proc;
  tbb::enumerable_thread_specific<int> t_sock;

  auto random_port = []{
    srand(time(nullptr));
    uint16_t lo = 49152;
    uint16_t hi = 65535;
    uint16_t x = lo + rand() % (hi - lo + 1);
    return static_cast<uint16_t>(x);
  };

  DaemonInitMessage dmsg{
    .base_port = random_port(),
    .n_conns = static_cast<uint16_t>(t_sock.size()),
  };
  std::atomic<uint16_t> cur_port = dmsg.base_port;

  auto recurse = [&](const auto& f, fs::path path, TrackedNode<M>* node) {
    if (!ok)
      return;

    int &sock = t_sock.local();
    if (sock == -1) {
      uint16_t port = cur_port.fetch_add(1);
      sock = connect_to_daemon(port);
    }

    uint32_t seq = g_seq;
    g_seq = (g_seq + 1) % std::numeric_limits<uint32_t>::max();

    if (!send_datamsg(sock, seq, base, path, node, ok)) {
      return report_async_error();
    }

    auto parallel_iter_children = [&] {
      tbb::task_group tbb;
      node->iter_children([&](TrackedNode<M>* child) {
        fs::path sub_path = path / child->meta.name;
        tbb.run([f, sub_path, child] { f(f, sub_path, child); });
      });
      tbb.wait();
    };

    switch (node->meta.ftype) {
      case fs::file_type::regular: {
        TrackedNode<M>* parent = node->parent;
        node->remove();
        bubble(parent);
        break;
      }
      case fs::file_type::directory: {
        size_t count = node->child_count();
        TrackedNode<M>* parent = node->parent;
        if (count == 0 && parent) { // Empty directory
          node->remove();
          bubble(parent);
        } else {
          pending.emplace(node, count);
          parallel_iter_children();
        }
        break;
      }
      default:
        fatal("impossible");
    }
  };

  recurse(recurse, root, root_node);

  // Check if the daemon was killed earlier.
  int exit_code = daemon.terminate();
  if (exit_code != 0) {
    report_stderr_remote(daemon);
    return false;
  }

  return ok;
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

static int enu_main(Args arg) {
  watch_dir(arg);

  {
    static TrackedNode<M> root_node{M{arg.path}, display_progress};
    signal(SIGINT, [](int) { clear_progress(&root_node); });

    std::jthread scan_thread = scan_dir(arg.path, &root_node);
    scan_thread.join();

    if (!sync_with_remote(arg.path, &root_node)) {
      clear_progress(&root_node);
      error("syncing error");
      return 1;
    }

    signal(SIGINT, SIG_DFL);
    clear_progress(&root_node);
  }

  return 0;
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

  if (arg.daemon)
    return !daemon_main();

  return enu_main(arg);
}
