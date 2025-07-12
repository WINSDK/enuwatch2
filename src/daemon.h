#pragma once

#include "log.h"
#include "net.h"
#include "sequencer.h"

#include <vector>
#include <sys/mman.h>

namespace enu {

struct DataMessage {
  uint32_t seq;
  fs::perms perms;
  fs::file_type ftype;
  uint16_t path_len;
  uint64_t fsize;
  char data[];
};

static std::vector<std::string> ANON_FILES;

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

static std::pair<int, std::string> create_anonfile(size_t fsize, mode_t mode) {
  register_tempfile_cleanup();

  char template_path[] = "/tmp/enu.XXXXXX";
  int fd = mkstemp(template_path);
  if (fd == -1)
    pfatal("mkstemp({})", fd);

  std::string s_template_path = ANON_FILES.emplace_back(std::string{template_path});

  if (ftruncate(fd, fsize) == -1)
    pfatal("ftruncate({})", fd);

  return {fd, s_template_path};
}

static bool copy_to_file(int in_fd, int out_fd, size_t fsize) {
  // Important as mmap(..) will fail on mapping 0-sized files.
  if (fsize == 0)
    return true;

  void* map = mmap(nullptr, fsize, PROT_WRITE, MAP_SHARED, out_fd, 0);
  if (map == MAP_FAILED)
    pfatal("mmap({})", out_fd);

  // Connection closed.
  if (!read_exact(in_fd, map, fsize))
    return false;

  return true;
}

/*
Executed on the host machine. One way listener for files/dirs to create

1. Read `SequencerMessage` from stdin over SSH connection
*/
static bool daemon_main() {
  int in_fd = STDIN_FILENO;
  int sock = connect_to_sequencer();

  auto resolve_data_msg = [&](SequencerMessage& seq_msg, size_t fsize) {
    // Directory creation is passed along to sequencer instead.
    if (seq_msg.ftype == fs::file_type::regular) {
      auto mode = static_cast<mode_t>(seq_msg.perms); // Seems fine.
      auto [out_fd, anon_path] = create_anonfile(fsize, mode);

      if (!copy_to_file(in_fd, out_fd, fsize)) {
        close(out_fd);
        return false;
      }

      // Attach now created file to be moved later.
      seq_msg.anon_path = anon_path;
      close(out_fd);
    }

    return true;
  };

  while (true) {
    DataMessage hdr;
    if (!read_exact(in_fd, &hdr, sizeof(hdr)))
      return false;

    std::string path(hdr.path_len, '\0');
    if (!read_exact(in_fd, path.data(), hdr.path_len))
      return false;

    SequencerMessage seq_msg{hdr.seq, hdr.ftype, hdr.perms, std::move(path)};
    if (!resolve_data_msg(seq_msg, hdr.fsize))
      return false;

    if (!write_frame(sock, seq_msg))
      return false;
  }

  return true;
}

}
