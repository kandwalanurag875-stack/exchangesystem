// gen_itch.cpp — Milestone 8: write a byte-accurate NASDAQ ITCH 5.0 file.
//
// Produces a realistic, NON-crossing order-by-order stream (bids below the mid,
// asks above) with adds / executions / cancels / deletes / replaces, so the
// reconstructed book stays clean. Lets you exercise the binary parser without
// downloading NASDAQ's multi-GB samples — point itch_replay at the output (or
// at a real `*.bin` sample; same code path).
//
//   gen_itch [--out data/sample.itch] [--events N] [--seed S]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <unordered_map>
#include <vector>

#include "me/itch_writer.h"
#include "me/types.h"

using namespace me;

struct Live { Side side; Price price; std::uint32_t shares; };

int main(int argc, char** argv) {
    const char* out = "data/sample.itch";
    std::size_t events = 200000;
    std::uint64_t seed = 0x1234;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--events") && i + 1 < argc) events = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
    }

    ItchWriter w;
    w.systemEvent('O');                 // start of messages
    w.stockDirectory("AAPL");
    w.systemEvent('S');                 // start of system hours

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::uniform_int_distribution<std::uint32_t> qd(1, 200);
    const Price mid = 100000;           // $10.0000 in 1/10000 ticks
    std::normal_distribution<double> off(0, 20);

    std::unordered_map<std::uint64_t, Live> live;
    std::vector<std::uint64_t> refs;
    std::uint64_t next_ref = 1;

    auto pick = [&]() -> std::uint64_t {
        if (refs.empty()) return 0;
        std::uniform_int_distribution<std::size_t> p(0, refs.size() - 1);
        return refs[p(rng)];
    };
    auto remove_ref = [&](std::uint64_t ref) {
        live.erase(ref);
        for (std::size_t i = 0; i < refs.size(); ++i)
            if (refs[i] == ref) { refs[i] = refs.back(); refs.pop_back(); break; }
    };

    for (std::size_t i = 0; i < events; ++i) {
        const double r = u(rng);
        if (r < 0.60 || refs.empty()) {                       // ADD
            const Side side = (u(rng) < 0.5) ? Side::Buy : Side::Sell;
            Price d = static_cast<Price>(std::abs(off(rng))) + 1;
            const Price px = (side == Side::Buy) ? (mid - d) : (mid + d);
            const std::uint32_t sh = qd(rng);
            const std::uint64_t ref = next_ref++;
            w.addOrder(ref, side, sh, px);
            live[ref] = {side, px, sh};
            refs.push_back(ref);
        } else if (r < 0.75) {                                // EXECUTE
            std::uint64_t ref = pick(); auto& o = live[ref];
            std::uint32_t ex = 1 + (qd(rng) % o.shares);
            w.execute(ref, ex);
            if (ex >= o.shares) remove_ref(ref); else o.shares -= ex;
        } else if (r < 0.85) {                                // CANCEL (partial)
            std::uint64_t ref = pick(); auto& o = live[ref];
            std::uint32_t cx = 1 + (qd(rng) % o.shares);
            w.cancel(ref, cx);
            if (cx >= o.shares) remove_ref(ref); else o.shares -= cx;
        } else if (r < 0.93) {                                // DELETE (full)
            std::uint64_t ref = pick();
            w.del(ref);
            remove_ref(ref);
        } else {                                              // REPLACE
            std::uint64_t old_ref = pick(); Live o = live[old_ref];
            Price d = static_cast<Price>(std::abs(off(rng))) + 1;
            const Price px = (o.side == Side::Buy) ? (mid - d) : (mid + d);
            const std::uint32_t sh = qd(rng);
            const std::uint64_t nref = next_ref++;
            w.replace(old_ref, nref, sh, px);
            remove_ref(old_ref);
            live[nref] = {o.side, px, sh};
            refs.push_back(nref);
        }
    }
    w.systemEvent('C');                 // end of messages

    std::FILE* f = std::fopen(out, "wb");
    if (!f) { std::printf("cannot open %s for writing\n", out); return 1; }
    std::fwrite(w.bytes().data(), 1, w.size(), f);
    std::fclose(f);
    std::printf("wrote %s : %zu bytes, %zu events, %zu orders still resting\n",
                out, w.size(), events, live.size());
    return 0;
}
