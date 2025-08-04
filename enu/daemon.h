#pragma once

#include "log.h"
#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <cstring>

namespace enu {

namespace fs = std::filesystem;
namespace chrono = std::chrono;

struct DataMessage {
  uint32_t seq;
  fs::perms perms;
  fs::file_type ftype;
  uint16_t path_len;
  uint64_t fsize;
  char data[];
};

static std::unordered_set<std::string> ANON_FILES;

static void register_tempfile_cleanup() {
  static bool registered = false;
  if (!registered) {
    atexit([] {
      for (std::string_view path : ANON_FILES) {
        unlink(path.data());
      }
    });
    registered = true;
  }
};

static std::pair<int, const std::string*> create_anonfile(size_t fsize, mode_t mode) {
  register_tempfile_cleanup();

  char template_path[] = "/tmp/enu.XXXXXX";
  int fd = mkstemp(template_path);
  if (fd == -1)
    pfatal("mkstemp({})", fd);

  if (ftruncate(fd, fsize) == -1)
    pfatal("ftruncate({})", fd);

  const std::string* s_template_path = &*ANON_FILES.emplace(template_path).first;
  return {fd, s_template_path};
}

static void move_anonfile(const std::string* anon_path, std::string_view perm_path) {
  if (rename(anon_path->data(), perm_path.data()) == -1)
    pfatal("rename({}, {})", *anon_path, perm_path);

  ANON_FILES.erase(*anon_path);
}

struct DaemonInitMessage {
  uint16_t base_port;
  uint16_t n_conns;
};

static void set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    pfatal("fcntl(fd, F_GETFL, 0)");
  flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (flags == -1)
    error("couldn't set socket as non-blocking");
}

static int bind_daemon(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1)
    pfatal("socket()");

  int optval = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
    pfatal("setsockopt({})", fd);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
    pfatal("bind({})", fd);

  if (listen(fd, 1) == -1) // backlog=1 as we are only accepting one connection.
    pfatal("listen({})", fd);

  set_nonblocking(fd);

  return fd;
}

// Blocking connect with retry/backoff.
static int connect_to_daemon(uint16_t port, in_addr_t addr = htonl(INADDR_LOOPBACK)) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
    pfatal("socket()");

  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = addr;

  auto format_ipv4 = [&] {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    return std::format("{}:{}", ip, ntohs(sa.sin_port));
  };

  auto delay = chrono::milliseconds{1};
  for (size_t attempt = 0; attempt < 5; ++attempt) {
    if (connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0)
      return fd;

    if (errno != ECONNREFUSED && errno != ENETUNREACH &&
        errno != EHOSTUNREACH && errno != ETIMEDOUT && errno != EINTR) {
      pfatal("connect({})", format_ipv4());
    }

    std::this_thread::sleep_for(delay);
    delay *= 2;
  }

  pfatal("connect({}) - retries exhausted", format_ipv4());
}

enum class ConnectionKind {
  Connected,
  HeaderP1,
  HeaderP2,
  Receiving,
};

struct ConnectionState {
  ConnectionKind kind = ConnectionKind::Connected;
  uint32_t seq;

  mode_t perms;
  std::string path;

  size_t progress = 0;
  size_t goal;

  const std::string* anon_path;
  int anon_fd = -1;
  u_int8_t* anon_buf;

  std::vector<uint8_t> buf;
  int client_fd = -1;
  int sock_fd = -1;

  void reset() {
    close(anon_fd);
    anon_fd = -1;
    progress = 0;
    goal = 0;
    buf.clear();
  }

  ~ConnectionState() {
    if (anon_fd != -1)
      close(anon_fd);
    if (client_fd != -1)
      close(client_fd);
    if (sock_fd != -1)
      close(sock_fd);
  }
};

enum class Progress {
  Fail,
  Partial,
  Done,
};

/*
1. Receives BASE_PORT and N channels over STDIN
2. Listens on ports BASE_PORT..=BASE_PORT+N
3. Each port is has a state and we will poll all sockets.
4. We will only consume a socket's buffer when there are sizeof(DataMessage) bytes received.
*/
static bool daemon_main() {
  DaemonInitMessage dmsg;
  if (!read_exact(STDIN_FILENO, &dmsg, sizeof(dmsg)))
    return false;

  std::vector<pollfd> fds;
  fds.reserve(dmsg.n_conns * 2);

  std::unordered_set<int> socket_fds;
  socket_fds.reserve(dmsg.n_conns);

  for (uint16_t conn = 0; conn < dmsg.n_conns; ++conn) {
    int sock = bind_daemon(dmsg.base_port + conn);
    fds.emplace_back(sock, POLLIN, 0);
    socket_fds.insert(sock);
  }

  std::unordered_map<int, ConnectionState> states;
  states.reserve(dmsg.n_conns);

  while (true) {
    int n_fds = poll(fds.data(), fds.size(), -1); // Wait indefinitely.
    if (n_fds == -1) {
      if (errno == EINTR)
        continue;
      pfatal("poll");
    }

    for (ssize_t idx = 0; idx < fds.size(); ++idx) {
      pollfd& pfd = fds[idx];

      // We allow clients to disconnect, even if that results in incomplete transfers.
      if (pfd.revents & (POLLHUP | POLLERR)) {
        states.erase(pfd.fd);
        fds.erase(fds.begin() + idx);
        --idx; // We don't want to skip past the next event.
        continue;
      }

      if ((pfd.revents & POLLIN) == 0) {
        warn("unexpected revent from poll: {:x}", pfd.revents);
        continue;
      }

      if (socket_fds.contains(pfd.fd)) {
        // We aren't accepting more than the number of expected connections.
        if (fds.size() > dmsg.n_conns * 2)
          pfatal("one too many connections accepted");

        int client_fd = accept(pfd.fd, nullptr, nullptr);
        if (client_fd == -1)
          pfatal("accept({})", pfd.fd);

        fds.emplace_back(client_fd, POLLIN, 0);
        ConnectionState& state = states[client_fd];
        state.client_fd = client_fd;
        state.sock_fd = pfd.fd;
        continue;
      }

      ConnectionState& state = states[pfd.fd];
      auto advance = [&](uint8_t* target) {
        IOResult r = read_partial(pfd.fd, target + state.progress, state.goal - state.progress);
        if (!r)
          return Progress::Fail;
        state.progress += r.n_bytes;
        return state.progress == state.goal ? Progress::Done : Progress::Partial;
      };

      if (state.kind == ConnectionKind::Connected) {
        state.buf.resize(sizeof(DataMessage));
        state.kind = ConnectionKind::HeaderP1;
        state.goal = sizeof(DataMessage);
      }

      // We have have not yet received a `DataMessage` and so are waiting for header
      if (state.kind == ConnectionKind::HeaderP1) {
        Progress p = advance(state.buf.data());
        if (p == Progress::Fail)
          return false;
        if (p == Progress::Done) {
          auto msg = reinterpret_cast<DataMessage*>(state.buf.data());

          state.goal += msg->path_len;
          state.buf.resize(state.goal);
          state.kind = ConnectionKind::HeaderP2;
        }
      }

      if (state.kind == ConnectionKind::HeaderP2) {
        Progress p = advance(state.buf.data());
        if (p == Progress::Fail)
          return false;
        if (p == Progress::Done) {
          auto msg = reinterpret_cast<DataMessage*>(state.buf.data());

          state.seq = msg->seq;
          state.path = std::string{msg->data, msg->path_len};
          state.perms = static_cast<mode_t>(state.perms); // Seems fine.

          if (msg->ftype == fs::file_type::regular) {
            auto [anon_fd, anon_path] = create_anonfile(msg->fsize, state.perms);
            state.anon_fd = anon_fd;
            state.anon_path = anon_path;
            state.progress = 0;
            state.goal = msg->fsize;

            if (msg->fsize > 0) {
              void* map = mmap(nullptr, msg->fsize, PROT_WRITE, MAP_SHARED, anon_fd, 0);
              if (map == MAP_FAILED)
                pfatal("mmap({})", anon_fd);
              state.anon_buf = static_cast<uint8_t*>(map);
            }

            state.kind = ConnectionKind::Receiving;
          } else {
            // NOTE: Don't actually do this here, we need to reorder the ops first.
            if (mkdir(state.path.c_str(), state.perms) == -1)
              pfatal("mkdir({})", state.path);

            state.reset();
            state.kind = ConnectionKind::Connected;
          }
        }
      }

      if (state.kind == ConnectionKind::Receiving) {
        Progress p = advance(state.anon_buf);
        if (p == Progress::Fail)
          return false;
        if (p == Progress::Done) {
          fsync(state.anon_fd);
          move_anonfile(state.anon_path, state.path);

          state.reset();
          state.kind = ConnectionKind::Connected;
        }
      }
    }
  }

  return true;
}

} // namespace enu
