#pragma once

#include <functional>
#include <string>
#include "base.h"

#ifdef ENU_MACOS
#include <CoreServices/CoreServices.h>
#endif

namespace enu::watcher {

enum class FsEventKind {
  created,
  deleted,
  modify,
  meta,
  moved,
  // We may have overflown event queue
  sync,
  // Ex: segment along path got deleted/moved.
  unrecoverable,
};

struct FsEvent {
  FsEventKind kind;
  std::string path;
  std::string dest;
};

using Handler = std::function<void(FsEvent)>;

class Watcher {
#ifdef ENU_MACOS
  dispatch_queue_t queue_;
  FSEventStreamRef stream_;
  Handler handler_;
#endif

 public:
  explicit Watcher(const std::string &path, double latency);
  void run_in_thread(Handler&& h);

  ~Watcher();

  Watcher(const Watcher&) = delete;
  Watcher operator=(const Watcher&) = delete;
};

}; // namespace enu::watcher
