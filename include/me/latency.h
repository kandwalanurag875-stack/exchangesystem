// latency.h — latency recording + percentile reporting for the benchmark
// (Milestone 2, Section 9). Shared by bench / bench_ds / pipeline_bench.
//
// Discipline (Section 9.1): we record into a PRE-ALLOCATED array during the
// measured loop and do NOT print, allocate, or compute statistics until after.
// Anything else pollutes the numbers. Percentiles, not averages (Section 9.2) —
// the tail is the story.
#ifndef ME_LATENCY_H
#define ME_LATENCY_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace me {

// Operation tags so we can break latency down by op type (adds, cancels and
// matches have very different costs — Section 9.2).
enum class Op : std::uint8_t { AddLimit = 0, AddMarket = 1, Cancel = 2, Modify = 3 };
inline const char* op_name(Op o) {
    switch (o) {
        case Op::AddLimit:  return "add-limit";
        case Op::AddMarket: return "add-market";
        case Op::Cancel:    return "cancel";
        case Op::Modify:    return "modify";
    }
    return "?";
}

class LatencyRecorder {
public:
    // Pre-size (and touch) the backing arrays so no allocation or page fault
    // happens during the measured loop.
    explicit LatencyRecorder(std::size_t capacity) {
        ns_.resize(capacity);
        op_.resize(capacity);
    }

    // Hot-path: a single store. `n` is already in nanoseconds.
    inline void record(Op op, std::uint32_t n) {
        ns_[count_] = n;
        op_[count_] = static_cast<std::uint8_t>(op);
        ++count_;
    }

    std::size_t size() const { return count_; }

    // ---- post-processing (called AFTER the measured loop) ----------------
    struct Stats {
        std::size_t   n = 0;
        std::uint32_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0, min = 0;
        double        mean = 0.0;
    };

    // Stats over all ops, or just those matching `only` when filter==true.
    Stats compute(bool filter = false, Op only = Op::AddLimit) const {
        std::vector<std::uint32_t> v;
        v.reserve(count_);
        double sum = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            if (filter && op_[i] != static_cast<std::uint8_t>(only)) continue;
            v.push_back(ns_[i]);
            sum += ns_[i];
        }
        Stats s;
        if (v.empty()) return s;
        std::sort(v.begin(), v.end());
        s.n    = v.size();
        s.min  = v.front();
        s.max  = v.back();
        s.mean = sum / static_cast<double>(v.size());
        s.p50  = pct(v, 0.50);
        s.p90  = pct(v, 0.90);
        s.p99  = pct(v, 0.99);
        s.p999 = pct(v, 0.999);
        return s;
    }

    void print_table(const char* title) const {
        std::printf("\n%-12s %10s %8s %8s %8s %8s %10s %9s\n",
                    title, "count", "p50", "p90", "p99", "p99.9", "max", "mean");
        std::printf("------------ ---------- -------- -------- -------- -------- ---------- ---------\n");
        print_row("ALL", compute());
        for (Op o : {Op::AddLimit, Op::AddMarket, Op::Cancel, Op::Modify}) {
            Stats s = compute(true, o);
            if (s.n) print_row(op_name(o), s);
        }
    }

    // Dump every sample to CSV for the matplotlib histogram (Section 9.3).
    bool dump_csv(const std::string& path) const {
        std::ofstream out(path);
        if (!out) return false;
        out << "op,ns\n";
        for (std::size_t i = 0; i < count_; ++i)
            out << op_name(static_cast<Op>(op_[i])) << ',' << ns_[i] << '\n';
        return true;
    }

private:
    static std::uint32_t pct(const std::vector<std::uint32_t>& sorted, double p) {
        if (sorted.empty()) return 0;
        // nearest-rank method on the sorted samples
        std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
        return sorted[idx];
    }
    static void print_row(const char* name, const Stats& s) {
        std::printf("%-12s %10zu %8u %8u %8u %8u %10u %9.1f\n",
                    name, s.n, s.p50, s.p90, s.p99, s.p999, s.max, s.mean);
    }

    std::vector<std::uint32_t> ns_;   // nanoseconds per op
    std::vector<std::uint8_t>  op_;   // Op tag per op
    std::size_t count_ = 0;
};

}  // namespace me

#endif  // ME_LATENCY_H
