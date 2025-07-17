#include "watcher.h"

#include <sys/inotify.h>

namespace enu::watcher {

Watcher::Watcher(const std::string &path, double latency) {
}

Watcher::~Watcher() {
}

void Watcher::run_in_thread(Handler&& h) {
}

} // namespace enu::watcher
