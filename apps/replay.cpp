// replay.cpp — Milestone 1 demo: "here's a working exchange core."
// Reads orders from a CSV (or generates synthetic flow), matches them, prints
// the resulting trades, and shows the final top-of-book. Verifies invariants.
//
//   replay <orders.csv>
//   replay --synthetic <N>            (default N=100000)
//   replay <orders.csv> --quiet       (suppress per-trade printing)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "me/csv.h"
#include "me/order_book.h"
#include "me/synthetic.h"

using namespace me;

static const char* side_str(Side s) { return s == Side::Buy ? "BUY " : "SELL"; }

int main(int argc, char** argv) {
    bool quiet = false;
    std::vector<Command> cmds;
    std::string source;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quiet") == 0) { quiet = true; }
        else if (std::strcmp(argv[i], "--synthetic") == 0) {
            std::size_t n = (i + 1 < argc) ? std::strtoull(argv[++i], nullptr, 10) : 100000;
            SynthParams p; p.count = n;
            cmds = generate(p);
            source = "synthetic(" + std::to_string(n) + ")";
        } else {
            cmds = load_csv(argv[i]);
            source = argv[i];
        }
    }

    if (cmds.empty()) {
        std::printf("usage: replay <orders.csv> | --synthetic <N> [--quiet]\n");
        return 1;
    }

    OrderBook book(cmds.size() + 16);

    std::uint64_t n_trades = 0, vol = 0;
    auto on_trade = [&](const Trade& t) {
        ++n_trades; vol += t.qty;
        if (!quiet)
            std::printf("TRADE #%llu  taker=%llu %s  maker=%llu  %llu @ %lld\n",
                        (unsigned long long)t.seq, (unsigned long long)t.taker_id,
                        side_str(t.taker_side), (unsigned long long)t.maker_id,
                        (unsigned long long)t.qty, (long long)t.price);
    };

    std::uint64_t accepted = 0;
    for (const auto& c : cmds)
        if (book.apply(c, on_trade)) ++accepted;

    Price bb = 0, ba = 0;
    const bool has_bid = book.bestBid(bb);
    const bool has_ask = book.bestAsk(ba);

    std::printf("\n--------------------------------------------------\n");
    std::printf("source         : %s\n", source.c_str());
    std::printf("commands        : %zu (accepted %llu)\n", cmds.size(),
                (unsigned long long)accepted);
    std::printf("trades          : %llu  (volume %llu)\n",
                (unsigned long long)n_trades, (unsigned long long)vol);
    std::printf("resting orders  : %zu\n", book.restingCount());
    if (has_bid) std::printf("best bid        : %lld\n", (long long)bb);
    if (has_ask) std::printf("best ask        : %lld\n", (long long)ba);
    if (has_bid && has_ask) std::printf("spread          : %lld\n", (long long)(ba - bb));

    std::vector<std::pair<Price, Qty>> levels;
    book.topAsks(5, levels);
    std::printf("\n        ASKS (top 5)\n");
    for (auto it = levels.rbegin(); it != levels.rend(); ++it)
        std::printf("   %8lld  x %llu\n", (long long)it->first,
                    (unsigned long long)it->second);
    book.topBids(5, levels);
    std::printf("        BIDS (top 5)\n");
    for (auto& lv : levels)
        std::printf("   %8lld  x %llu\n", (long long)lv.first,
                    (unsigned long long)lv.second);

    const bool ok = book.checkInvariants();
    std::printf("\ninvariants      : %s\n", ok ? "PASS" : "*** FAIL ***");
    return ok ? 0 : 2;
}
