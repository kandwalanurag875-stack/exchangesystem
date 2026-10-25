// itch_replay.cpp — Milestone 8: parse a NASDAQ ITCH 5.0 file and reconstruct
// the order book from the real wire protocol.
//
//   itch_replay <file.itch> [--limit N]
//
// Works on the output of gen_itch AND on a real NASDAQ TotalView-ITCH 5.0
// BinaryFILE sample (same 2-byte length framing). Reports message-type counts,
// the rebuilt top-of-book, and parsing+reconstruction throughput.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "me/itch.h"

using namespace me;

int main(int argc, char** argv) {
    const char* path = nullptr;
    std::uint64_t limit = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 10);
        else path = argv[i];
    }
    if (!path) { std::printf("usage: itch_replay <file.itch> [--limit N]\n"); return 1; }

    ItchBookBuilder builder(1u << 21);
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t n = itch_parse_file(path, builder, limit);
    const auto t1 = std::chrono::steady_clock::now();
    if (n == 0) { std::printf("no messages parsed from %s (missing or empty?)\n", path); return 1; }

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const auto& s = builder.stats();
    auto& book = builder.book();

    std::printf("\nITCH 5.0 replay: %s\n", path);
    std::printf("--------------------------------------------------\n");
    std::printf("messages parsed : %llu  in %.3f s  (%.2f M msg/s)\n",
                (unsigned long long)n, secs, n / secs / 1e6);
    std::printf("  add           : %llu\n", (unsigned long long)s.adds);
    std::printf("  execute       : %llu\n", (unsigned long long)s.executes);
    std::printf("  cancel        : %llu\n", (unsigned long long)s.cancels);
    std::printf("  delete        : %llu\n", (unsigned long long)s.deletes);
    std::printf("  replace       : %llu\n", (unsigned long long)s.replaces);
    std::printf("  trade msgs    : %llu\n", (unsigned long long)s.trades);
    std::printf("  skipped/admin : %llu\n", (unsigned long long)s.skipped);
    std::printf("resting orders  : %zu\n", book.restingCount());

    Price bb = 0, ba = 0;
    const bool hb = book.bestBid(bb), ha = book.bestAsk(ba);
    if (hb) std::printf("best bid        : %lld  ($%.4f)\n", (long long)bb, bb / 10000.0);
    if (ha) std::printf("best ask        : %lld  ($%.4f)\n", (long long)ba, ba / 10000.0);
    if (hb && ha) std::printf("spread          : %lld\n", (long long)(ba - bb));

    std::vector<std::pair<Price, Qty>> lv;
    book.topAsks(5, lv);
    std::printf("\n        ASKS (top 5)\n");
    for (auto it = lv.rbegin(); it != lv.rend(); ++it)
        std::printf("   %8lld  x %llu\n", (long long)it->first, (unsigned long long)it->second);
    book.topBids(5, lv);
    std::printf("        BIDS (top 5)\n");
    for (auto& l : lv)
        std::printf("   %8lld  x %llu\n", (long long)l.first, (unsigned long long)l.second);

    std::printf("\ninvariants      : %s\n", book.checkInvariants() ? "PASS" : "*** FAIL ***");
    return book.checkInvariants() ? 0 : 2;
}
