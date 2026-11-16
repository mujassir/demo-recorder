// Lock-free single-producer / single-consumer ring buffer.
//
// Spec rule for the recording pipeline: no allocation in capture callbacks and
// lock-free queues between threads. The buffer is allocated once up front;
// try_push and try_pop never allocate, lock or block.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

namespace dr {

template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "ring elements must be trivially copyable");

 public:
  // Capacity is rounded up to a power of two.
  explicit SpscRing(size_t min_capacity) {
    size_t cap = 2;
    while (cap < min_capacity) cap <<= 1;
    mask_ = cap - 1;
    slots_ = std::make_unique<T[]>(cap);
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  size_t capacity() const { return mask_ + 1; }

  // Producer thread only. Returns false when full (the item is dropped).
  bool try_push(const T& item) {
    const size_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_cache_ > mask_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head - tail_cache_ > mask_) return false;
    }
    slots_[head & mask_] = item;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only.
  bool try_pop(T& out) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_cache_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail == head_cache_) return false;
    }
    out = slots_[tail & mask_];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Approximate; exact only when both threads are idle.
  size_t size_approx() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }

 private:
  static constexpr size_t kLine = 64;

  std::unique_ptr<T[]> slots_;
  size_t mask_ = 0;
  // Producer-owned.
  alignas(kLine) std::atomic<size_t> head_{0};
  size_t tail_cache_ = 0;
  // Consumer-owned.
  alignas(kLine) std::atomic<size_t> tail_{0};
  size_t head_cache_ = 0;
};

}  // namespace dr
