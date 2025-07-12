#pragma once

#include "log.h"

#include <cstdint>
#include <optional>
#include <atomic>
#include <array>
#include <sys/shm.h>

namespace enu {

template <typename T>
struct ShmQueue;

template <typename T>
struct ShmReader {
  uint32_t tail_;
  const ShmQueue<T>* queue_;

  std::optional<T> pop();
};

template <typename T>
struct ShmQueue {
  static const int shmkey = 0x43576825;
  static const size_t SHM_SIZE = 1024 * 4;

  static_assert(std::is_copy_constructible_v<T>, "T must be copy-constructible");

  using ShmBuffer = std::array<uint32_t, SHM_SIZE / sizeof(uint32_t) - sizeof(uint32_t)>;

  alignas(64) std::atomic<uint32_t> head_;
  alignas(64) ShmBuffer buf_;

  static ShmQueue* open() {
    int shmid;
    if ((shmid = shmget(shmkey, SHM_SIZE, IPC_CREAT | IPC_R | IPC_W)) == -1)
      pfatal("shmget()");

    void* map;
    if ((map = shmat(shmid, NULL, 0)) == reinterpret_cast<void*>(-1))
      pfatal("shmat()");

    return reinterpret_cast<ShmQueue*>(map);
  }

  void push(uint64_t value) {
    size_t idx = head_.load(std::memory_order_acquire);
    buf_[idx % buf_.size()] = value;
    head_.store(idx + 1, std::memory_order_release);
  }

  ShmReader<T> reader() const {
    return ShmReader{head_.load(std::memory_order_acquire), this};
  }
};

template <typename T>
std::optional<T> ShmReader<T>::pop() {
  uint32_t head = queue_->head_.load(std::memory_order_relaxed);
  if (tail_ == head) // nothing new
    return false;

  T value = queue_->buf_[tail_ % queue_->buf_.size()];
  ++tail_;
  return value;
}

}
