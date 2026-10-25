// lobster.h — Milestone 8: parse LOBSTER message files (reconstructed NASDAQ
// limit-order-book data) and optionally CROSS-CHECK the rebuilt book against
// LOBSTER's own orderbook file (the Section 11 "reference cross-check").
//
// LOBSTER message file columns (one event per line):
//   Time, EventType, OrderID, Size, Price, Direction
//     EventType: 1=new limit order  2=partial cancel  3=full delete
//                4=execution of a visible limit order  5=execution of hidden
//                6=cross trade  7=trading halt
//     Direction: 1 = buy limit order (bid side)  -1 = sell limit order (ask)
//     Price    : integer dollars*10000 (we keep the integer tick directly)
//
// Reconstruction is order-by-order, no matching (the feed already says what
// happened):  1 -> rest order   2/3/4 -> reduce/remove the referenced order
// 5/6/7 -> not displayed-book events (skipped). A marketable incoming order is
// recorded by LOBSTER only as type-4 executions against the resting orders it
// hits, so type 1 always genuinely rests — the book never goes crossed.
#ifndef ME_LOBSTER_H
#define ME_LOBSTER_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "me/csv.h"
#include "me/order_book.h"
#include "me/types.h"

namespace me {

struct LobMsg {
    int           type = 0;
    OrderId       id = 0;
    Qty           size = 0;
    Price         price = 0;
    int           dir = 1;     // 1 buy, -1 sell
};

inline bool parse_lobster_line(const std::string& raw, LobMsg& out) {
    if (raw.empty() || raw[0] == '#') return false;
    auto f = split_commas(raw);
    if (f.size() < 6) return false;
    out.type  = std::atoi(f[1].c_str());
    out.id    = std::strtoull(f[2].c_str(), nullptr, 10);
    out.size  = std::strtoull(f[3].c_str(), nullptr, 10);
    out.price = std::strtoll(f[4].c_str(), nullptr, 10);
    out.dir   = std::atoi(f[5].c_str());
    return true;
}

struct LobStats {
    std::uint64_t total = 0, adds = 0, cancels = 0, deletes = 0,
                  executes = 0, skipped = 0;
};

// Reconstruct a book from a LOBSTER message stream.
class LobsterBook {
public:
    explicit LobsterBook(std::size_t pool = 1u << 20) : book_(pool) {}

    void apply(const LobMsg& m) {
        ++stats_.total;
        const Side side = (m.dir == 1) ? Side::Buy : Side::Sell;
        switch (m.type) {
            case 1: book_.loadResting(side, m.price, m.size, m.id); ++stats_.adds; break;
            case 2: book_.reduce(m.id, m.size); ++stats_.cancels; break;
            case 3: book_.cancel(m.id);          ++stats_.deletes; break;
            case 4: book_.reduce(m.id, m.size);  ++stats_.executes; break;
            default: ++stats_.skipped; break;    // 5 hidden, 6 cross, 7 halt
        }
    }

    OrderBook& book() { return book_; }
    const LobStats& stats() const { return stats_; }

private:
    OrderBook book_;
    LobStats stats_;
};

inline std::vector<LobMsg> load_lobster_messages(const std::string& path) {
    std::vector<LobMsg> v;
    std::ifstream in(path);
    std::string line; LobMsg m;
    while (std::getline(in, line))
        if (parse_lobster_line(line, m)) v.push_back(m);
    return v;
}

// ---- cross-check against a LOBSTER orderbook file ---------------------------
// The orderbook file has, per row, the top `levels` of the book as
//   ask_px_1, ask_sz_1, bid_px_1, bid_sz_1, ask_px_2, ...
// We compare our reconstructed top-N to row i after applying message i. LOBSTER
// pads missing levels with a sentinel price (a huge magnitude) and size 0; we
// treat size 0 as "empty" and skip it. Returns (rows_compared, mismatches).
struct CrossCheck { std::uint64_t rows = 0, mismatches = 0; };

// ---- aggregate L2 book for validating against real LOBSTER orderbook files --
// LOBSTER level data doesn't include the OPENING book (orders resting before the
// sample window), so replaying messages from empty can't rebuild the absolute
// book. The rigorous validation is PER-STEP: seed from the real orderbook row
// i-1, apply message i, and check it equals row i (then re-seed from the real
// row i). This tests message semantics against real data with zero drift.
struct L2Book {
    std::map<Price, Qty, std::greater<Price>> bids;   // best = begin()
    std::map<Price, Qty, std::less<Price>>    asks;   // best = begin()
    void add(Side s, Price p, Qty q) { if (s == Side::Buy) bids[p] += q; else asks[p] += q; }
    void reduce(Side s, Price p, Qty q) {
        if (s == Side::Buy) { auto it = bids.find(p); if (it != bids.end()) { if (q >= it->second) bids.erase(it); else it->second -= q; } }
        else                { auto it = asks.find(p); if (it != asks.end()) { if (q >= it->second) asks.erase(it); else it->second -= q; } }
    }
};

inline void l2_apply(L2Book& b, const LobMsg& m) {
    const Side s = (m.dir == 1) ? Side::Buy : Side::Sell;
    switch (m.type) {
        case 1: b.add(s, m.price, m.size); break;        // new limit order
        case 2: case 3: case 4: b.reduce(s, m.price, m.size); break;  // cancel/delete/exec
        default: break;                                  // 5 hidden, 6 cross, 7 halt
    }
}

inline void parse_orderbook_row(const std::vector<std::string>& row, int levels, L2Book& out) {
    out.bids.clear(); out.asks.clear();
    for (int k = 0; k < levels; ++k) {
        const std::size_t base = static_cast<std::size_t>(k) * 4;
        if (base + 3 >= row.size()) break;
        const Price apx = std::strtoll(row[base + 0].c_str(), nullptr, 10);
        const Qty   asz = std::strtoull(row[base + 1].c_str(), nullptr, 10);
        const Price bpx = std::strtoll(row[base + 2].c_str(), nullptr, 10);
        const Qty   bsz = std::strtoull(row[base + 3].c_str(), nullptr, 10);
        if (asz > 0) out.asks[apx] = asz;   // size 0 => empty/padded level
        if (bsz > 0) out.bids[bpx] = bsz;
    }
}

// Compare the top-k of both sides. Returns true iff identical to that depth.
inline bool l2_match(const L2Book& a, const L2Book& b, int k) {
    auto cmp = [&](auto ia, auto ea, auto ib, auto eb) {
        int c = 0;
        for (; c < k && ia != ea && ib != eb; ++ia, ++ib, ++c)
            if (ia->first != ib->first || ia->second != ib->second) return false;
        if (c < k && ((ia != ea) != (ib != eb))) return false;   // one ran out early
        return true;
    };
    return cmp(a.bids.begin(), a.bids.end(), b.bids.begin(), b.bids.end()) &&
           cmp(a.asks.begin(), a.asks.end(), b.asks.begin(), b.asks.end());
}

inline bool top_levels_match(OrderBook& book, const std::vector<std::string>& row,
                             int levels) {
    std::vector<std::pair<Price, Qty>> bids, asks;
    book.topAsks(static_cast<std::size_t>(levels), asks);
    book.topBids(static_cast<std::size_t>(levels), bids);
    for (int i = 0; i < levels; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * 4;
        if (base + 3 >= row.size()) break;
        const Price a_px = std::strtoll(row[base + 0].c_str(), nullptr, 10);
        const Qty   a_sz = std::strtoull(row[base + 1].c_str(), nullptr, 10);
        const Price b_px = std::strtoll(row[base + 2].c_str(), nullptr, 10);
        const Qty   b_sz = std::strtoull(row[base + 3].c_str(), nullptr, 10);
        if (a_sz != 0) {
            if (i >= (int)asks.size() || asks[i].first != a_px || asks[i].second != a_sz)
                return false;
        }
        if (b_sz != 0) {
            if (i >= (int)bids.size() || bids[i].first != b_px || bids[i].second != b_sz)
                return false;
        }
    }
    return true;
}

}  // namespace me

#endif  // ME_LOBSTER_H
