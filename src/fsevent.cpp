#include <CoreFoundation/CFArray.h>
#include <CoreServices/CoreServices.h>
#include <print>
#include "log.h"
#include "watcher.h"

namespace enu::watcher {

void event_callback(
  ConstFSEventStreamRef,
  void* handler_ptr,
  size_t event_count,
  void* event_paths_ptr,
  const FSEventStreamEventFlags flags[],
  const FSEventStreamEventId[]) {
  auto handler = *static_cast<Handler*>(handler_ptr);
  auto event_paths = static_cast<CFArrayRef>(event_paths_ptr);

  std::vector<FsEvent> events;
  // Detect rename chains in an event batch macOS may reuse the inode of a
  // deleted file for a newly created one, so make sure to poll events quickly.
  std::unordered_map<unsigned long, std::string> chained;

  for (long idx = 0; idx < event_count; ++idx) {
    auto path_info_dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(event_paths, idx));

    auto cf_path = static_cast<CFStringRef>(
      CFDictionaryGetValue(path_info_dict, kFSEventStreamEventExtendedDataPathKey));

    auto cf_inode = static_cast<CFNumberRef>(
      CFDictionaryGetValue(path_info_dict, kFSEventStreamEventExtendedFileIDKey));

    CFIndex len = CFStringGetLength(cf_path);
    CFIndex max_path_size = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;

    std::string path(max_path_size, '\0');
    if (!CFStringGetCString(cf_path, path.data(), max_path_size, kCFStringEncodingUTF8)) {
      fatal("CFStringGetCString failed on decoding event path");
    }

    unsigned long inode;
    CFNumberGetValue(cf_inode, kCFNumberLongType, &inode);

    if (
      flags[idx] &
      (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount |
       kFSEventStreamEventFlagUnmount)) {
      events.push_back({FsEventKind::unrecoverable});
      continue; // Can't have any other events when the root is gone
    }

    if (
      flags[idx] & (kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagMustScanSubDirs)) {
      events.push_back({FsEventKind::sync});
      continue; // No use in handling other events when total resync is required
    }

    if (flags[idx] & kFSEventStreamEventFlagItemCreated)
      events.push_back({FsEventKind::created, path});

    if (flags[idx] & kFSEventStreamEventFlagItemRenamed) {
      auto it = chained.find(inode);
      if (it != chained.end()) {
        // Since the this inode has been previously moved, we must be the target of the move
        std::string& src = it->second;
        events.push_back({FsEventKind::moved, std::move(src), path});
        chained.erase(it);
      }

      chained[inode] = path;
    }

    if (
      flags[idx] &
      (kFSEventStreamEventFlagItemChangeOwner | kFSEventStreamEventFlagItemInodeMetaMod))
      events.push_back({FsEventKind::meta, path});

    if (flags[idx] & kFSEventStreamEventFlagItemRemoved)
      events.push_back({FsEventKind::deleted, std::move(path)});
  }

  for (FsEvent& e : events)
    handler(std::move(e));
}

FSEventStreamRef create_stream(CFArrayRef pathsToWatch, double latency, Handler* handler) {
  FSEventStreamContext context{};
  context.info = handler;

  FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagNone;

  flags |= kFSEventStreamCreateFlagNoDefer; // idk
  flags |= kFSEventStreamCreateFlagUseExtendedData;
  flags |= kFSEventStreamCreateFlagUseCFTypes;
  flags |= kFSEventStreamCreateFlagFileEvents;

  FSEventStreamRef stream = FSEventStreamCreate(
    nullptr,
    &event_callback,
    &context,
    pathsToWatch,
    kFSEventStreamEventIdSinceNow,
    latency,
    flags);

  if (!stream)
    fatal("FSEventStreamCreate shouldn't fail, old version of MacOS?");

  return stream;
}

Watcher::Watcher(const std::string &path, double latency) {
  queue_ = dispatch_queue_create("enu_fsevent_queue", nullptr);

  CFStringRef c_path =
    CFStringCreateWithCString(kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
  CFArrayRef paths_to_watch =
    CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void**>(&c_path), 1, nullptr);
  stream_ = create_stream(paths_to_watch, latency, &handler_);

  FSEventStreamSetDispatchQueue(stream_, queue_);
}

void Watcher::run_in_thread(Handler&& h) {
  handler_ = h; // Pointer to handler_ was given earlier in FSEventStreamContext.

  if (!FSEventStreamStart(stream_))
    fatal("FSEventStreamStart shouldn't fail, old version of MacOS?");
}

Watcher::~Watcher() {
  FSEventStreamStop(stream_);
  FSEventStreamInvalidate(stream_);
  FSEventStreamRelease(stream_);

  dispatch_release(queue_);
}

} // namespace enu::watcher
