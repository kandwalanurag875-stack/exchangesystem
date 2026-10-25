// bench.cpp — Milestone 2: the benchmark harness + latency histogram data.
//
// Measures PER-OPERATION latency (rdtsc bracketing only the apply() call, with
// recording done outside the timed region) and end-to-end throughput, then
// reports percentiles (Section 9.2) and dumps a CSV for the matplotlib
// histogram (Section 9.3). No averages-only, no fabricated numbers — these are
// whatever your machine actually does.
//
//   bench [--orders N] [--data synthetic|<csv>] [--seed S] [--csv docs/bench.csv]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "me/cpuinfo.h"
#include "me/csv.h"
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

int main(int argc, char** argv) {
    std::size_t orders = 1'000'000;
    std::string data = "synthetic";
    std::string csv_out = "docs/bench.csv";
    std::uint64_t seed = 0xC0FFEE;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--orders") && i + 1 < argc)
            orders = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc)
            data = argv[++i];
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc)
            csv_out = argv[++i];
    }

    // ---- build the workload ---------------------------------------------
    std::vector<Command> cmds;
    if (data == "synthetic") {
        SynthParams p; p.count = orders; p.seed = seed;
        cmds = generate(p);
    } else {
        cmds = load_csv(data);
        orders = cmds.size();
    }
    if (cmds.empty()) { std::printf("no commands\n"); return 1; }

    std::size_t n_add = 0, n_cancel = 0, n_market = 0, n_modify = 0;
    for (auto& c : cmds) {
        switch (classify(c)) {
            case Op::AddLimit:  ++n_add; break;
            case Op::AddMarket: ++n_market; break;
            case Op::Cancel:    ++n_cancel; break;
            case Op::Modify:    ++n_modify; break;
        }
    }

    // ---- calibrate the cycle counter ------------------------------------
    const double cpns = calibrate_cycles_per_ns();

    // ---- warm-up (NOT recorded): warms i-cache, branch predictor --------
    {
        OrderBook warm(cmds.size() + 16);
        auto sink = [](const Trade&) {};
        const std::size_t wn = cmds.size() < 200000 ? cmds.size() : 200000;
        for (std::size_t i = 0; i < wn; ++i) warm.apply(cmds[i], sink);
    }

    // ---- measured run ----------------------------------------------------
    LatencyRecorder rec(cmds.size());                 // pre-allocated
    OrderBook book(cmds.size() + 16);                 // pool pre-sized, pages touched
    std::uint64_t n_trades = 0, vol = 0;
    auto sink = [&](const Trade& t) { ++n_trades; vol += t.qty; };

    const auto wall0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const Op tag = classify(cmds[i]);
        const std::uint64_t a = rdtsc();
        book.apply(cmds[i], sink);
        const std::uint64_t b = rdtsc();              // brackets ONLY apply()
        std::uint64_t ns = static_cast<std::uint64_t>((b - a) / cpns);
        if (ns > 0xFFFFFFFFull) ns = 0xFFFFFFFFull;
        rec.record(tag, static_cast<std::uint32_t>(ns));  // outside timed region
    }
    const auto wall1 = std::chrono::steady_clock::now();

    const double secs =
        std::chrono::duration_cast<std::chrono::duration<double>>(wall1 - wall0).count();
    const double tput = static_cast<double>(cmds.size()) / secs;

    // ---- report (Section 9.5 shape) -------------------------------------
    const auto all = rec.compute();
    auto pctf = [&](std::size_t v) { return 100.0 * static_cast<double>(v) / cmds.size(); };
    std::printf("\n");
    std::printf("Engine:     single-writer, RB-tree (std::map) price levels\n");
    std::printf("Machine:    %s\n", cpu_brand().c_str());
    std::printf("Build:      C++17, %s\n",
#if defined(_MSC_VER)
                "MSVC /O2 /GR-");
#else
                "g++ -O3 -march=native");
#endif
    std::printf("Orders:     %zu  (%.0f%% add, %.0f%% cancel, %.0f%% market, %.0f%% modify)\n",
                cmds.size(), pctf(n_add), pctf(n_cancel), pctf(n_market), pctf(n_modify));
    std::printf("Trades:     %llu  (volume %llu)\n",
                (unsigned long long)n_trades, (unsigned long long)vol);
    std::printf("Wall time:  %.3f s\n", secs);
    std::printf("Throughput: %.3f M orders/sec\n", tput / 1e6);
    std::printf("Latency(ns): p50=%u  p90=%u  p99=%u  p99.9=%u  max=%u\n",
                all.p50, all.p90, all.p99, all.p999, all.max);

    rec.print_table("op breakdown");

    if (rec.dump_csv(csv_out))
        std::printf("\nCSV written: %s   ->  python tools/plot_hist.py %s\n",
                    csv_out.c_str(), csv_out.c_str());
    else
        std::printf("\n(could not write CSV to %s — does the docs/ dir exist?)\n",
                    csv_out.c_str());
    return 0;
}
