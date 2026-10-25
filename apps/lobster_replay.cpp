// lobster_replay.cpp — Milestone 8: replay a LOBSTER message file and validate
// the reconstruction against LOBSTER's own orderbook file using real NASDAQ data.
//
//   lobster_replay <_message_*.csv> [--orderbook <_orderbook_*.csv> --levels K]
//
// Validation method (PER-STEP). LOBSTER level data omits the opening book
// (orders resting before the sample window), so replaying messages from empty
// can't rebuild the absolute book. Instead we test message SEMANTICS: seed from
// the real orderbook row i-1, apply message i, and check it equals row i — then
// re-seed from the real row i. Zero drift; every one of the ~400k real events is
// checked. Get free samples at https://lobsterdata.com (or the mirrored HF
// dataset totalorganfailure/lobster-data).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "me/lobster.h"

using namespace me;

int main(int argc, char** argv) {
    const char* msg_path = nullptr;
    const char* ob_path = nullptr;
    int levels = 10;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--orderbook") && i + 1 < argc) ob_path = argv[++i];
        else if (!std::strcmp(argv[i], "--levels") && i + 1 < argc) levels = std::atoi(argv[++i]);
        else msg_path = argv[i];
    }
    if (!msg_path) {
        std::printf("usage: lobster_replay <message.csv> [--orderbook <ob.csv> --levels K]\n");
        return 1;
    }

    auto msgs = load_lobster_messages(msg_path);
    if (msgs.empty()) { std::printf("no LOBSTER messages in %s\n", msg_path); return 1; }

    std::uint64_t n1 = 0, n2 = 0, n3 = 0, n4 = 0, nskip = 0;
    for (auto& m : msgs) switch (m.type) {
        case 1: ++n1; break; case 2: ++n2; break; case 3: ++n3; break;
        case 4: ++n4; break; default: ++nskip; break;
    }

    std::printf("\nLOBSTER replay: %s\n", msg_path);
    std::printf("--------------------------------------------------\n");
    std::printf("messages        : %zu\n", msgs.size());
    std::printf("  add    (1)    : %llu\n", (unsigned long long)n1);
    std::printf("  cancel (2)    : %llu\n", (unsigned long long)n2);
    std::printf("  delete (3)    : %llu\n", (unsigned long long)n3);
    std::printf("  execute(4)    : %llu\n", (unsigned long long)n4);
    std::printf("  skipped(5-7)  : %llu\n", (unsigned long long)nskip);

    if (!ob_path) {
        // No reference file: just reconstruct through the engine's book (works
        // for a self-contained stream, e.g. the bundled hand sample).
        LobsterBook lb(1u << 20);
        for (auto& m : msgs) lb.apply(m);
        Price bb = 0, ba = 0;
        if (lb.book().bestBid(bb)) std::printf("best bid        : %lld\n", (long long)bb);
        if (lb.book().bestAsk(ba)) std::printf("best ask        : %lld\n", (long long)ba);
        std::printf("(pass --orderbook <file> to validate against the LOBSTER reference)\n");
        return 0;
    }

    // ---- per-step validation against the real orderbook file ----------------
    std::ifstream ob(ob_path);
    if (!ob) { std::printf("could not open orderbook file %s\n", ob_path); return 1; }

    std::string line;
    if (!std::getline(ob, line)) { std::printf("orderbook file empty\n"); return 1; }
    L2Book prev; parse_orderbook_row(split_commas(line), levels, prev);

    std::uint64_t steps = 0, miss_l1 = 0, miss_top = 0, miss_full = 0;
    const int topK = levels < 5 ? levels : 5;
    for (std::size_t k = 1; k < msgs.size(); ++k) {
        if (!std::getline(ob, line)) break;
        L2Book real; parse_orderbook_row(split_commas(line), levels, real);

        L2Book pred = prev;                 // start from the REAL previous book
        l2_apply(pred, msgs[k]);            // apply this one message
        ++steps;
        if (!l2_match(pred, real, 1))      ++miss_l1;
        if (!l2_match(pred, real, topK))   ++miss_top;
        if (!l2_match(pred, real, levels)) ++miss_full;  // full depth (boundary-sensitive)
        prev = real;                        // re-seed from real data (no drift)
    }

    auto rate = [](std::uint64_t miss, std::uint64_t n) {
        return n ? 100.0 * (n - miss) / n : 0.0;
    };
    std::printf("\nper-step validation vs real LOBSTER orderbook file:\n");
    std::printf("  steps checked : %llu\n", (unsigned long long)steps);
    std::printf("  best bid/ask (L1) match : %.4f%%  (%llu mismatches)\n",
                rate(miss_l1, steps), (unsigned long long)miss_l1);
    std::printf("  top-%d match            : %.4f%%  (%llu mismatches)\n",
                topK, rate(miss_top, steps), (unsigned long long)miss_top);
    std::printf("  top-%d (full) match     : %.4f%%  (%llu mismatches; depth-%d boundary)\n",
                levels, rate(miss_full, steps), (unsigned long long)miss_full, levels);
    return 0;
}
