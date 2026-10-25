// platform.h — portability shims so the engine builds identically on
// Linux/g++ (the deploy target in the spec) and Windows/MSVC (the dev box).
//
// We deliberately keep this tiny. The hot path code never #ifdefs; all the
// OS/compiler divergence is funnelled through the helpers below.
#ifndef ME_PLATFORM_H
#define ME_PLATFORM_H

#include <cstdint>
#include <cstdio>
#include <chrono>

// ---- branch hints -----------------------------------------------------------
// __builtin_expect on GCC/Clang feeds the static branch predictor. MSVC has no
// equivalent in C++17 ( [[likely]] is C++20 ), so it degrades to a no-op.
#if defined(__GNUC__) || defined(__clang__)
#  define ME_LIKELY(x)   (__builtin_expect(!!(x), 1))
#  define ME_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#  define ME_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#  define ME_LIKELY(x)   (x)
#  define ME_UNLIKELY(x) (x)
#  define ME_ALWAYS_INLINE __forceinline
#endif

// Cache line size used for alignas() padding to kill false sharing (Sec 7.2).
namespace me {
constexpr std::size_t kCacheLine = 64;
}

// A polite spin-wait hint for busy-wait loops in the lock-free ring buffer.
// PAUSE yields the hyperthread and saves power/avoids pipeline flushes.
#if defined(_MSC_VER)
#  include <intrin.h>
#endif
namespace me {
ME_ALWAYS_INLINE void cpu_pause() {
#if defined(_MSC_VER)
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    /* no-op on other ISAs */
#endif
}
}

// ---- durable file flush (used by the WAL, Milestone 5) ----------------------
// fsync() forces the OS page cache to physical storage. On Windows the
// equivalent is _commit() on the underlying file descriptor.
#if defined(_WIN32)
#  include <io.h>
namespace me {
inline int file_no(std::FILE* f) { return _fileno(f); }
inline int fsync_fd(int fd)       { return _commit(fd); }
}
#else
#  include <unistd.h>
namespace me {
inline int file_no(std::FILE* f) { return ::fileno(f); }
inline int fsync_fd(int fd)       { return ::fsync(fd); }
}
#endif

namespace me {
// Flush stdio buffer AND force the OS to persist to disk. Returns 0 on success.
inline int durable_flush(std::FILE* f) {
    if (std::fflush(f) != 0) return -1;      // userspace buffer -> OS
    return fsync_fd(file_no(f));             // OS cache -> platter/SSD
}
}

// ---- high resolution cycle counter (used by the benchmark, Milestone 2) -----
// On x86-64 rdtsc gives a cheap, fine-grained cycle counter (finer than the
// ~100ns granularity of QueryPerformanceCounter on Windows). We calibrate it
// against std::chrono once at startup to convert cycles -> nanoseconds. Modern
// CPUs have an invariant TSC, so the conversion factor is stable across
// frequency scaling. We document this caveat in docs/bench.md.
#if defined(_MSC_VER)
#  include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#  include <x86intrin.h>
#endif

namespace me {
ME_ALWAYS_INLINE std::uint64_t rdtsc() {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    return static_cast<std::uint64_t>(__rdtsc());
#else
    // Portable fallback for non-x86 (e.g. ARM): steady_clock in ns.
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Calibrate cycles-per-nanosecond by busy-spinning rdtsc over a fixed wall
// interval. Call once; cache the result.
inline double calibrate_cycles_per_ns() {
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::uint64_t c0 = rdtsc();
    // Spin ~50ms — long enough to average out noise, short enough to be cheap.
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now() - t0).count() < 50) { /* busy wait */ }
    const std::uint64_t c1 = rdtsc();
    const auto t1 = clock::now();
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns;
#else
    return 1.0;  // fallback already returns ns
#endif
}
}

#endif  // ME_PLATFORM_H
