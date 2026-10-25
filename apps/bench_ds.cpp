// bench_ds.cpp — Milestone 3: the data-structure benchmark (the "measured
// engineering decision" story). Runs the SAME workload through:
//    (A) OrderBook  — std::map (Red-Black tree) price levels
//    (B) ArrayBook  — flat std::vector indexed by price tick
// then (1) cross-checks they produce byte-identical trades + final book
// (Section 11 reference cross-check) and (2) compares latency + throughput.
//
//   bench_ds [--orders N] [--seed S]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "me/array_book.h"
#include "me/cpuinfo.h"
#include "me/latency.h"
#include "me/order_book.h"
#include "me/platform.h"
#include "me/synthetic.h"

using namespace me;

static Op classify(const Command& c) {
    if (c.type == CmdType::Cancel) return Op::Cancel;
    if (c.type == CmdType::Modify) return Op::Modify;
    if (c.ord_type == OrderType::Market) return Op::AddMarket;
    return Op::AddLimit;
}

// Rolling 64-bit FNV-1a hash of the trade stream — lets us prove two engines
// produced identical executions without storing millions of trades.
struct TradeDigest {
    std::uint64_t h = 1469598103934665603ull;
    std::uint64_t count = 0, vol = 0;
    void mix(std::uint64_t x) { h ^= x; h *= 1099511628211ull; }
    void operator()(const Trade& t) {
        mix(t.taker_id); mix(t.maker_id);
        mix(static_cast<std::uint64_t>(t.price)); mix(t.qty);
        ++count; vol += t.qty;
    }
};

template <class Book>
static double run(Book& book, const std::vector<Command>& cmds,
                  LatencyRecorder& rec, TradeDigest& dig, double cpns) {
    const auto w0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const Op tag = classify(cmds[i]);
        const std::uint64_t a = rdtsc();
        book.apply(cmds[i], dig);
        const std::uint64_t b = rdtsc();
        std::uint64_t ns = static_cast<std::uint64_t>((b - a) / cpns);
        if (ns > 0xFFFFFFFFull) ns = 0xFFFFFFFFull;
        rec.record(tag, static_cast<std::uint32_t>(ns));
    }
    const auto w1 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::duration<double>>(w1 - w0).count();
}

int main(int argc, char** argv) {
    std::size_t orders = 1'000'000;
    std::uint64_t seed = 0xC0FFEE;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--orders") && i + 1 < argc)
            orders = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = std::strtoull(argv[++i], nullptr, 10);
    }

    SynthParams p; p.count = orders; p.seed = seed;
    auto cmds = generate(p);

    // Size the ArrayBook's price band to cover every limit price in the stream
    // (so it rejects nothing and parity with OrderBook is exact).
    Price lo = p.mid, hi = p.mid;
    for (auto& c : cmds)
        if (c.type == CmdType::Add && c.ord_type == OrderType::Limit) {
            if (c.price < lo) lo = c.price;
            if (c.price > hi) hi = c.price;
        }
    lo -= 8; hi += 8;
    std::printf("workload: %zu cmds, price band [%lld, %lld] (%lld ticks)\n",
                cmds.size(), (long long)lo, (long long)hi, (long long)(hi - lo + 1));

    const double cpns = calibrate_cycles_per_ns();

    // warm-up both (discarded)
    { OrderBook w(cmds.size()+16); TradeDigest d; auto s=[&](const Trade&t){d(t);};
      for (std::size_t i=0;i<cmds.size() && i<200000;++i) w.apply(cmds[i], s); }

    // ---- (A) RB-tree -----------------------------------------------------
    LatencyRecorder recA(cmds.size());
    TradeDigest digA;
    OrderBook rb(cmds.size() + 16);
    const double secA = run(rb, cmds, recA, digA, cpns);
    const std::string serA = rb.serialize();

    // ---- (B) flat array --------------------------------------------------
    LatencyRecorder recB(cmds.size());
    TradeDigest digB;
    ArrayBook arr(lo, hi, cmds.size() + 16);
    const double secB = run(arr, cmds, recB, digB, cpns);
    const std::string serB = arr.serialize();

    // ---- cross-check (correctness BEFORE comparing speed) ----------------
    const bool trades_match = (digA.h == digB.h && digA.count == digB.count &&
                               digA.vol == digB.vol);
    const bool book_match = (serA == serB);
    std::printf("\nCROSS-CHECK (RB-tree vs flat-array on identical input):\n");
    std::printf("  trades:     %s  (count=%llu vol=%llu, hashes %s)\n",
                trades_match ? "IDENTICAL" : "*** DIVERGED ***",
                (unsigned long long)digA.count, (unsigned long long)digA.vol,
                digA.h == digB.h ? "equal" : "DIFFER");
    std::printf("  final book: %s\n", book_match ? "IDENTICAL" : "*** DIVERGED ***");
    if (arr.rejected())
        std::printf("  (array rejected %zu out-of-band orders!)\n", arr.rejected());

    // ---- speed comparison -------------------------------------------------
    std::printf("\nMachine: %s\n", cpu_brand().c_str());
    auto sa = recA.compute(); auto sb = recB.compute();
    std::printf("\n%-16s %12s %8s %8s %8s %8s %10s\n",
                "structure", "Morders/s", "p50", "p90", "p99", "p99.9", "max");
    std::printf("---------------- ------------ -------- -------- -------- -------- ----------\n");
    std::printf("%-16s %12.3f %8u %8u %8u %8u %10u\n", "RB-tree(std::map)",
                cmds.size()/secA/1e6, sa.p50, sa.p90, sa.p99, sa.p999, sa.max);
    std::printf("%-16s %12.3f %8u %8u %8u %8u %10u\n", "flat-array",
                cmds.size()/secB/1e6, sb.p50, sb.p90, sb.p99, sb.p999, sb.max);

    recA.print_table("RB-tree ops");
    recB.print_table("array ops");

    const double speedup = secA / secB;
    std::printf("\nVERDICT: flat-array is %.2fx %s on throughput "
                "(p50 %u ns vs %u ns).\n",
                speedup >= 1.0 ? speedup : 1.0 / speedup,
                speedup >= 1.0 ? "FASTER" : "SLOWER",
                sb.p50, sa.p50);
    std::printf("See docs/ds_compare.md for the why.\n");

    return (trades_match && book_match) ? 0 : 2;
}
