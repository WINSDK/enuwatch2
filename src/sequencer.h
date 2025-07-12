#pragma once

#include "log.h"
#include "net.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <thread>

namespace fs = std::filesystem;
namespace chrono = std::chrono;

namespace enu {

constexpr std::string_view IPC_SOCK_ADDR = "/tmp/enu_ipc";

// Public API for constructing a `SequencerFrame`.
struct SequencerMessage {
  uint32_t seq;
  fs::file_type ftype;
  fs::perms perms;
  std::string target_path;
  std::string anon_path;
};

// [[gnu::packed]] fucks things up here.
struct SequencerFrame {
  uint32_t seq;
  fs::file_type ftype;
  fs::perms perms;
  uint16_t target_path_len;
  uint16_t anon_path_len;
  char data[];

  const char* target_path() const {
    return data;
  }

  const char* anon_path() const {
    return &data[target_path_len];
  }
};

using UniqueSequencerFrame = std::unique_ptr<SequencerFrame, void (*)(void*)>;

static std::optional<UniqueSequencerFrame> read_frame(int fd) {
  // Read frame header first to determine `frame.data` length.
  SequencerFrame hdr;
  if (!read_exact(fd, &hdr, sizeof(hdr)))
    return {};

  size_t data_len = hdr.target_path_len + hdr.anon_path_len;
  size_t frame_size = sizeof(hdr) + data_len;

  auto deleter = [](void* p) { ::operator delete[](p, std::align_val_t{alignof(SequencerFrame)}); };

  // Aligned allocation of complete `SequencerFrame`.
  void* raw = ::operator new[](frame_size, std::align_val_t{alignof(SequencerFrame)});
  auto frame = static_cast<SequencerFrame*>(raw);
  memcpy(frame, &hdr, sizeof(hdr));

  if (!read_exact(fd, frame->data, data_len))
    return {};

  return std::unique_ptr<SequencerFrame, void (*)(void*)>{frame, deleter};
}

static bool write_frame(int fd, const SequencerMessage& msg) {
  SequencerFrame hdr;
  hdr.seq = msg.seq;
  hdr.ftype = msg.ftype;
  hdr.perms = msg.perms;
  hdr.target_path_len = msg.target_path.size() + 1; // +1 for null-termination
  hdr.anon_path_len = msg.anon_path.size() + 1;

  if (!write_exact(fd, &hdr, sizeof(hdr)))
    return false;

  if (!write_exact(fd, msg.target_path.c_str(), hdr.target_path_len))
    return false;

  if (!write_exact(fd, msg.anon_path.c_str(), hdr.anon_path_len))
    return false;

  return true;
}

static int sequencer_bind() {
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd == -1)
    pfatal("socket()");

  // In case it already exists.
  unlink(IPC_SOCK_ADDR.data());

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", IPC_SOCK_ADDR.data());

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
    pfatal("bind({})", IPC_SOCK_ADDR);

  return fd;
}

static int connect_to_sequencer() {
  const chrono::milliseconds TIMEOUT{300};
  chrono::microseconds interval{500}; // Doubling intervals every attempt.

  auto with_timeout = [&](auto&& f) {
    auto start_time = chrono::steady_clock::now();
    while (true) {
      if (auto result = f(); result.has_value())
        return result.value();

      auto elapsed = chrono::steady_clock::now() - start_time;
      if (elapsed >= TIMEOUT)
        fatal("connect({}) - timeout after {}ms", IPC_SOCK_ADDR, TIMEOUT.count());

      std::this_thread::sleep_for(interval);
      interval *= 2;
    }
  };

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd == -1)
    pfatal("socket()");

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", IPC_SOCK_ADDR.data());

  return with_timeout([&]() -> std::optional<int> {
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
      return fd;

    return {};
  });
}

static bool apply_frame(const SequencerFrame* frame) {
  switch (frame->ftype) {
    case fs::file_type::regular: {
      if (rename(frame->anon_path(), frame->target_path()) == -1)
        pfatal("rename({}, {})", frame->anon_path(), frame->target_path());
      break;
    }
    case fs::file_type::directory: {
      auto mode = static_cast<mode_t>(frame->perms); // Seems fine.
      if (mkdir(frame->target_path(), mode) == -1) {
        if (errno == EEXIST)
          fatal("directory already exists: {}", frame->target_path());
        error("mkdir({}): {}", frame->target_path(), strerror(errno));
        return false;
      }
      break;
    }
    default:
      fatal("unexpected frame type {}", static_cast<int>(frame->ftype));
  }

  return true;
}

/*
Executed on the host machine.
Receives actions from different daemons on the local network, out of order.

1. Listen on `IPC_SOCK_ADDR` for modifications to commit
2. Re-order messages by sequencer number
3. Apply the appropriate actions in order
*/
static bool sequencer_main() {
  int recv_sock = sequencer_bind();

  uint32_t next_seq = 0;
  std::map<uint32_t, UniqueSequencerFrame> pending;

  while (true) {
    std::optional<UniqueSequencerFrame> oframe = read_frame(recv_sock);
    if (!oframe)
      return false;

    SequencerFrame* frame = oframe->get();
    apply_frame(frame);
    pending.emplace(frame->seq, std::move(*oframe));

    while (true) {
      auto it = pending.find(next_seq);
      if (it == pending.end())
        break;

      SequencerFrame* it_frame = it->second.get();
      if (!apply_frame(it_frame))
        return false;

      pending.erase(it);
      ++next_seq;
    }
  }

  return pending.empty();
}

} // namespace enu
