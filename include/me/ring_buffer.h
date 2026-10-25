// ring_buffer.h — Milestone 4: a lock-free SPSC ring buffer, the C++17
// translation of the LMAX Disruptor's core idea (papers #1 and #3).
//
// Why this instead of std::mutex + std::queue (Section 7.1):
//   * Pre-allocated, bounded array — ZERO allocation on the hot path. A
//     std::queue grows with new/delete per element; that is the cardinal sin.
//   * No locks. Coordination is two atomic sequence counters, not a mutex. No
//     syscalls, no kernel arbitration, no priority inversion.
//   * Single-producer / single-consumer: the matching thread is the lone
//     consumer (the single-writer principle) and reads commands strictly in
//     order, which keeps the engine deterministic.
//
// Mechanical sympathy (Section 7.2):
//   * size is a power of two => wrap index is `seq & (size-1)`, a bitwise AND,
//     never a modulo.
//   * head_ and tail_ live on SEPARATE cache lines (alignas(64)) so the
//     producer writing head_ doesn't invalidate the consumer's line — no false
//     sharing.
//   * Each side caches the other's cursor (head_cache_/tail_cache_) so the
//     common case touches only its own cache line and avoids a cross-core
//     atomic load on every operation (the Disruptor's "batching" insight).
#ifndef ME_RING_BUFFER_H
#define ME_RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "me/platform.h"

namespace me {

template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity_pow2)
        : mask_(capacity_pow2 - 1), buf_(capacity_pow2) {
        // power-of-two capacity is required for the bitwise-AND wrap.
        // (assert-style guard; capacity 0 or non-pow2 is a programmer error.)
    }

    // Producer side. Returns false if the ring is full (caller decides to spin).
    bool try_push(const T& v) {
        const std::uint64_t h = head_.seq.load(std::memory_order_relaxed);
        if (h - tail_cache_.seq >= mask_ + 1) {           // maybe full?
            tail_cache_.seq = tail_.seq.load(std::memory_order_acquire);
            if (h - tail_cache_.seq >= mask_ + 1) return false;  // truly full
        }
        buf_[h & mask_] = v;
        head_.seq.store(h + 1, std::memory_order_release); // publish
        return true;
    }

    // Consumer side. Returns false if the ring is empty.
    bool try_pop(T& out) {
        const std::uint64_t t = tail_.seq.load(std::memory_order_relaxed);
        if (t == head_cache_.seq) {                        // maybe empty?
            head_cache_.seq = head_.seq.load(std::memory_order_acquire);
            if (t == head_cache_.seq) return false;        // truly empty
        }
        out = buf_[t & mask_];
        tail_.seq.store(t + 1, std::memory_order_release); // free the slot
        return true;
    }

    // Blocking variants for the simple producer/consumer driver. They busy-spin
    // with a PAUSE hint; `done` lets the consumer exit once drained.
    void push_blocking(const T& v) {
        while (!try_push(v)) cpu_pause();
    }
    bool pop_blocking(T& out, const std::atomic<bool>& producing_done) {
        for (;;) {
            if (try_pop(out)) return true;
            if (producing_done.load(std::memory_order_acquire)) {
                if (try_pop(out)) return true;             // drain race
                return false;                              // empty AND done
            }
            cpu_pause();
        }
    }

    std::size_t capacity() const { return mask_ + 1; }

private:
    // Each cursor sits alone on a 64-byte line: no false sharing between the
    // producer's head_ and the consumer's tail_.
    struct alignas(kCacheLine) Cursor {
        std::atomic<std::uint64_t> seq{0};
    };
    struct alignas(kCacheLine) PlainCursor {
        std::uint64_t seq{0};
    };

    const std::uint64_t mask_;
    std::vector<T> buf_;                 // pre-allocated once
    Cursor head_;                        // written by producer
    Cursor tail_;                        // written by consumer
    PlainCursor tail_cache_;             // producer's private cache of tail_
    PlainCursor head_cache_;             // consumer's private cache of head_
};

}  // namespace me

#endif  // ME_RING_BUFFER_H
