#pragma once

#include "log.h"

#include <unistd.h>
#include <algorithm>
#include <cstddef>
#include <limits>

namespace enu {

enum class IOKind {
  Ok,
  Fail,
};

struct IOResult {
  IOKind kind = IOKind::Ok;
  size_t n_bytes = 0;

  operator bool() {
    return kind == IOKind::Ok;
  }
};

const size_t MAX_RW_SIZE = std::numeric_limits<int>::max();

static IOResult write_partial(int fd, const void* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    // read/write's can operate on `INT_MAX` bytes at a time.
    size_t chunk = std::min(MAX_RW_SIZE, len - off);
    ssize_t b_wrote = write(fd, reinterpret_cast<const uint8_t*>(buf) + off, chunk);
    if (b_wrote == 0)
      break;
    if (b_wrote < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      // error("write(fd={})", fd);
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
    size_t chunk = std::min(MAX_RW_SIZE, len - off);
    ssize_t b_read = read(fd, reinterpret_cast<uint8_t*>(buf) + off, chunk);
    if (b_read == 0)
      break;
    if (b_read < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      // error("read(fd={})", fd);
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

} // namespace enu
