#pragma once

// Lock-free single-producer / single-consumer byte ring, safe with one
// producer core and one consumer core on RP2350. Capacity must be a power
// of two. Same pattern as mimicusb.

#include <cstdint>

namespace testrig {

template <uint32_t CAP>
class SpscRing {
  static_assert((CAP & (CAP - 1)) == 0, "CAP must be a power of two");

 public:
  uint32_t push(const uint8_t* src, uint32_t len) {
    uint32_t head = head_;  // producer owns head
    uint32_t tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    uint32_t n = 0;
    while (n < len && (uint32_t)(head - tail) < CAP) {
      buf_[head & (CAP - 1)] = src[n++];
      head++;
    }
    __atomic_store_n(&head_, head, __ATOMIC_RELEASE);
    return n;
  }

  uint32_t pop(uint8_t* dst, uint32_t len) {
    uint32_t tail = tail_;  // consumer owns tail
    uint32_t head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    uint32_t n = 0;
    while (n < len && tail != head) {
      dst[n++] = buf_[tail & (CAP - 1)];
      tail++;
    }
    __atomic_store_n(&tail_, tail, __ATOMIC_RELEASE);
    return n;
  }

  bool empty() const {
    return __atomic_load_n(&head_, __ATOMIC_ACQUIRE) ==
           __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
  }

  // Consumer-side discard of everything currently queued.
  void drain() {
    uint32_t head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
    __atomic_store_n(&tail_, head, __ATOMIC_RELEASE);
  }

 private:
  uint8_t buf_[CAP];
  volatile uint32_t head_ = 0;
  volatile uint32_t tail_ = 0;
};

}  // namespace testrig
