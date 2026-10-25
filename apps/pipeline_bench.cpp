// pipeline_bench.cpp — Milestone 4: the LMAX Disruptor story.
//
// The paper's title is literally "a high performance alternative to bounded
// queues." So we measure exactly that: the SAME producer/consumer pipeline over
//   (A) std::mutex + condition_variable bounded queue   (the textbook baseline)
//   (B) our lock-free SpscRing                          (the Disruptor pattern)
//
// Two experiments:
//   1. RAW HANDOFF — push/pop N integers with trivial work. Isolates the queue
//      mechanism itself: this is where lock-free crushes the mutex.
//   2. FULL PIPELINE — producer feeds order commands, a single matching/writer
//      thread applies them to the OrderBook (single-writer principle). End-to-
//      end throughput, plus the consumer's per-op matching latency (which should
//      match the in-thread Milestone 2 number — the ring adds ~nothing per op).
//
//   pipeline_bench [--orders N] [--ring-bits B]   (ring capacity = 1<<B)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "me/cpuinfo.h"
#include "me/latency.h"
#include "me/order_book.h"
#include "me/platform.h"
#include "me/ring_buffer.h"
#include "me/synthetic.h"

using namespace me;

// ---- baseline: a lock-based bounded queue with the same interface ----------
template <class T>
class MutexBoundedQueue {
public:
    explicit MutexBoundedQueue(std::size_t cap_pow2)
        : mask_(cap_pow2 - 1), buf_(cap_pow2) {}

    void push_blocking(const T& v) {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return size_() < mask_ + 1; });
        buf_[head_ & mask_] = v;
        ++head_;
        lk.unlock();
        not_empty_.notify_one();
    }
    bool pop_blocking(T& out, const std::atomic<bool>& done) {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return size_() > 0 || done.load(); });
        if (size_() > 0) {
            out = buf_[tail_ & mask_];
            ++tail_;
            lk.unlock();
            not_full_.notify_one();
            return true;
        }
        return false;  // done and empty
    }
    std::size_t capacity() const { return mask_ + 1; }

private:
    std::size_t size_() const { return head_ - tail_; }
    std::uint64_t mask_;
    std::vector<T> buf_;
    std::uint64_t head_ = 0, tail_ = 0;
    std::mutex m_;
    std::condition_variable not_full_, not_empty_;
};

// ---- experiment 1: raw handoff ---------------------------------------------
template <class Queue>
static double raw_handoff(std::size_t n, std::size_t cap) {
    Queue q(cap);
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> sink{0};

    const auto t0 = std::chrono::steady_clock::now();
    std::thread consumer([&] {
        std::uint64_t v, acc = 0;
        while (q.pop_blocking(v, done)) acc += v;
        sink.store(acc);
    });
    for (std::uint64_t i = 0; i < n; ++i) q.push_blocking(i);
    done.store(true);
    consumer.join();
    const auto t1 = std::chrono::steady_clock::now();

    // touch sink so the loop isn't optimised away
    if (sink.load() == 0xDEADBEEF) std::printf(" ");
    return std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
}

// ---- experiment 2: full pipeline -------------------------------------------
template <class Queue>
static double full_pipeline(const std::vector<Command>& cmds, std::size_t cap,
                            double cpns, LatencyRecorder* rec,
                            std::uint64_t* out_trades) {
    Queue q(cap);
    std::atomic<bool> done{false};
    OrderBook book(cmds.size() + 16);
    std::uint64_t n_trades = 0;

    const auto t0 = std::chrono::steady_clock::now();
    std::thread matcher([&] {
        auto sink = [&](const Trade&) { ++n_trades; };
        Command c;
        while (q.pop_blocking(c, done)) {
            if (rec) {
                const std::uint64_t a = rdtsc();
                book.apply(c, sink);
                const std::uint64_t b = rdtsc();
                std::uint64_t ns = static_cast<std::uint64_t>((b - a) / cpns);
                if (ns > 0xFFFFFFFFull) ns = 0xFFFFFFFFull;
                Op tag = c.type == CmdType::Cancel ? Op::Cancel
                       : c.type == CmdType::Modify ? Op::Modify
                       : c.ord_type == OrderType::Market ? Op::AddMarket
                                                         : Op::AddLimit;
                rec->record(tag, static_cast<std::uint32_t>(ns));
            } else {
                book.apply(c, sink);
            }
        }
    });
    for (const auto& c : cmds) q.push_blocking(c);
    done.store(true);
    matcher.join();
    const auto t1 = std::chrono::steady_clock::now();

    *out_trades = n_trades;
    return std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count();
}

int main(int argc, char** argv) {
    std::size_t orders = 1'000'000;
    unsigned ring_bits = 16;  // capacity 65536
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--orders") && i + 1 < argc)
            orders = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--ring-bits") && i + 1 < argc)
            ring_bits = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    }
    const std::size_t cap = std::size_t(1) << ring_bits;

    SynthParams p; p.count = orders;
    auto cmds = generate(p);
    const double cpns = calibrate_cycles_per_ns();

    std::printf("Machine: %s\n", cpu_brand().c_str());
    std::printf("Threads: 1 producer + 1 matching/writer; ring capacity=%zu\n\n", cap);

    // ---- experiment 1: raw handoff (warm each once, then measure) ----------
    raw_handoff<MutexBoundedQueue<std::uint64_t>>(orders / 4, cap);
    raw_handoff<SpscRing<std::uint64_t>>(orders / 4, cap);
    const double mq = raw_handoff<MutexBoundedQueue<std::uint64_t>>(orders, cap);
    const double rg = raw_handoff<SpscRing<std::uint64_t>>(orders, cap);
    std::printf("== Experiment 1: raw handoff of %zu items ==\n", orders);
    std::printf("  mutex+condvar queue : %8.1f M items/sec  (%.3f s)\n", orders / mq / 1e6, mq);
    std::printf("  lock-free SpscRing  : %8.1f M items/sec  (%.3f s)\n", orders / rg / 1e6, rg);
    std::printf("  -> lock-free is %.1fx faster at pure handoff\n\n", mq / rg);

    // ---- experiment 2: full pipeline ---------------------------------------
    std::uint64_t tr_mq = 0, tr_rg = 0;
    full_pipeline<MutexBoundedQueue<Command>>(cmds, cap, cpns, nullptr, &tr_mq); // warm
    const double pmq = full_pipeline<MutexBoundedQueue<Command>>(cmds, cap, cpns, nullptr, &tr_mq);

    LatencyRecorder rec(cmds.size());
    full_pipeline<SpscRing<Command>>(cmds, cap, cpns, nullptr, &tr_rg);          // warm
    const double prg = full_pipeline<SpscRing<Command>>(cmds, cap, cpns, &rec, &tr_rg);

    std::printf("== Experiment 2: full pipeline (producer -> matching writer) ==\n");
    std::printf("  trades: mutex=%llu  ring=%llu  %s\n",
                (unsigned long long)tr_mq, (unsigned long long)tr_rg,
                tr_mq == tr_rg ? "(identical)" : "(*** MISMATCH ***)");
    std::printf("  mutex+condvar queue : %8.3f M orders/sec  (%.3f s)\n", orders / pmq / 1e6, pmq);
    std::printf("  lock-free SpscRing  : %8.3f M orders/sec  (%.3f s)\n", orders / prg / 1e6, prg);
    std::printf("  -> lock-free is %.2fx faster end-to-end\n", pmq / prg);

    rec.print_table("matching latency under the ring pipeline");
    std::printf("\n(The per-op matching latency above should match the in-thread\n"
                " Milestone 2 number: the ring adds ~no per-op cost; its win is\n"
                " removing the lock and decoupling ingest from matching.)\n");
    return tr_mq == tr_rg ? 0 : 2;
}
