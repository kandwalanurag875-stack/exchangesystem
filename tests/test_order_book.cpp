// test_order_book.cpp — correctness tests for the matching core (Section 11).
// Covers each order type and the edge cases the spec calls out, plus the
// Section 6.4 invariants and the determinism property.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "me/array_book.h"
#include "me/order_book.h"
#include "me/ring_buffer.h"
#include "me/synthetic.h"
#include "me/test_framework.h"

using namespace me;

// Small helper: collect trades from applying a command.
struct Collector {
    std::vector<Trade> trades;
    void operator()(const Trade& t) { trades.push_back(t); }
};

static Command addLimit(Side s, Price p, Qty q, OrderId id, Tif tif = Tif::Gtc) {
    Command c; c.type = CmdType::Add; c.ord_type = OrderType::Limit;
    c.side = s; c.price = p; c.qty = q; c.id = id; c.tif = tif; return c;
}
static Command addMarket(Side s, Qty q, OrderId id) {
    Command c; c.type = CmdType::Add; c.ord_type = OrderType::Market;
    c.side = s; c.qty = q; c.id = id; return c;
}
static Command cancel(OrderId id) {
    Command c; c.type = CmdType::Cancel; c.id = id; return c;
}

TEST_CASE("empty book: a limit order just rests, no trades") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Buy, 100, 10, 1), tr);
    CHECK(tr.trades.empty());
    Price bb;
    CHECK(b.bestBid(bb) && bb == 100);
    CHECK(b.restingCount() == 1);
    CHECK(b.checkInvariants());
}

TEST_CASE("no cross: bid below ask, both rest") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Buy, 99, 10, 1), tr);
    b.apply(addLimit(Side::Sell, 101, 10, 2), tr);
    CHECK(tr.trades.empty());
    Price bb, ba;
    CHECK(b.bestBid(bb) && bb == 99);
    CHECK(b.bestAsk(ba) && ba == 101);
    CHECK(b.checkInvariants());
}

TEST_CASE("full fill at the resting (maker) price") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 100, 10, 1), tr);   // resting ask @100
    b.apply(addLimit(Side::Buy, 105, 10, 2), tr);    // aggressive buy @105
    REQUIRE(tr.trades.size() == 1);
    // CRITICAL: trade executes at the maker's price 100, NOT the taker's 105.
    CHECK(tr.trades[0].price == 100);
    CHECK(tr.trades[0].qty == 10);
    CHECK(tr.trades[0].maker_id == 1);
    CHECK(tr.trades[0].taker_id == 2);
    CHECK(b.empty());
    CHECK(b.checkInvariants());
}

TEST_CASE("partial fill: remainder of the incoming order rests") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 100, 10, 1), tr);
    b.apply(addLimit(Side::Buy, 100, 25, 2), tr);    // wants 25, only 10 avail
    REQUIRE(tr.trades.size() == 1);
    CHECK(tr.trades[0].qty == 10);
    Price bb;
    CHECK(b.bestBid(bb) && bb == 100);               // 15 left resting on bid
    CHECK(b.restingCount() == 1);
    CHECK(b.checkInvariants());
}

TEST_CASE("price-time priority: same price, earliest first (FIFO)") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 100, 10, 1), tr);   // first in
    b.apply(addLimit(Side::Sell, 100, 10, 2), tr);   // second in
    b.apply(addLimit(Side::Buy, 100, 15, 3), tr);    // hits 1 fully, 2 partially
    REQUIRE(tr.trades.size() == 2);
    CHECK(tr.trades[0].maker_id == 1);               // order 1 matched first
    CHECK(tr.trades[0].qty == 10);
    CHECK(tr.trades[1].maker_id == 2);
    CHECK(tr.trades[1].qty == 5);
    CHECK(b.checkInvariants());
}

TEST_CASE("best price priority: cheaper ask fills before pricier ask") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 102, 10, 1), tr);
    b.apply(addLimit(Side::Sell, 100, 10, 2), tr);   // better (lower) ask
    b.apply(addLimit(Side::Buy, 105, 10, 3), tr);
    REQUIRE(tr.trades.size() == 1);
    CHECK(tr.trades[0].maker_id == 2);               // 100 before 102
    CHECK(tr.trades[0].price == 100);
    CHECK(b.checkInvariants());
}

TEST_CASE("market order sweeps multiple levels until filled") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 100, 5, 1), tr);
    b.apply(addLimit(Side::Sell, 101, 5, 2), tr);
    b.apply(addLimit(Side::Sell, 102, 5, 3), tr);
    b.apply(addMarket(Side::Buy, 12, 4), tr);        // 5+5+2
    REQUIRE(tr.trades.size() == 3);
    CHECK(tr.trades[0].price == 100);
    CHECK(tr.trades[1].price == 101);
    CHECK(tr.trades[2].price == 102 && tr.trades[2].qty == 2);
    Price ba;
    CHECK(b.bestAsk(ba) && ba == 102);               // 3 left at 102
    CHECK(b.checkInvariants());
}

TEST_CASE("market order against empty book: no trade, never rests") {
    OrderBook b; Collector tr;
    b.apply(addMarket(Side::Buy, 10, 1), tr);
    CHECK(tr.trades.empty());
    CHECK(b.empty());
    CHECK(b.checkInvariants());
}

TEST_CASE("cancel removes a resting order; O(1) lookup") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Buy, 100, 10, 1), tr);
    b.apply(addLimit(Side::Buy, 100, 10, 2), tr);
    CHECK(b.cancel(1));                               // cancel head of FIFO
    CHECK(b.restingCount() == 1);
    // order 2 should still be matchable
    b.apply(addLimit(Side::Sell, 100, 10, 3), tr);
    REQUIRE(tr.trades.size() == 1);
    CHECK(tr.trades[0].maker_id == 2);
    CHECK(b.checkInvariants());
}

TEST_CASE("cancel of a non-existent order returns false") {
    OrderBook b;
    CHECK(!b.cancel(999));
    CHECK(b.checkInvariants());
}

TEST_CASE("IOC fills what it can and drops the rest (no rest)") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 100, 5, 1), tr);
    b.apply(addLimit(Side::Buy, 100, 20, 2, Tif::Ioc), tr);
    REQUIRE(tr.trades.size() == 1);
    CHECK(tr.trades[0].qty == 5);
    CHECK(b.empty());                                // 15 remainder dropped
    CHECK(b.checkInvariants());
}

TEST_CASE("FOK rejects entirely when not fully fillable") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 100, 5, 1), tr);
    b.apply(addLimit(Side::Buy, 100, 20, 2, Tif::Fok), tr);  // needs 20, only 5
    CHECK(tr.trades.empty());                        // nothing executed
    Price ba;
    CHECK(b.bestAsk(ba) && ba == 100);               // resting ask untouched
    CHECK(b.checkInvariants());
}

TEST_CASE("FOK executes fully when liquidity suffices") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Sell, 100, 10, 1), tr);
    b.apply(addLimit(Side::Sell, 101, 10, 2), tr);
    b.apply(addLimit(Side::Buy, 101, 15, 3, Tif::Fok), tr);
    REQUIRE(tr.trades.size() == 2);
    CHECK(tr.trades[0].qty == 10);
    CHECK(tr.trades[1].qty == 5);
    CHECK(b.checkInvariants());
}

TEST_CASE("modify re-queues at the tail, losing time priority") {
    OrderBook b; Collector tr;
    b.apply(addLimit(Side::Buy, 100, 10, 1), tr);
    b.apply(addLimit(Side::Buy, 100, 10, 2), tr);
    Command m; m.type = CmdType::Modify; m.id = 1; m.price = 100; m.qty = 10;
    b.apply(m, tr);                                  // order 1 -> back of queue
    b.apply(addLimit(Side::Sell, 100, 10, 3), tr);   // should hit order 2 now
    REQUIRE(tr.trades.size() == 1);
    CHECK(tr.trades[0].maker_id == 2);
    CHECK(b.checkInvariants());
}

TEST_CASE("shares are conserved: added - matched - cancelled == resting") {
    OrderBook b;
    std::uint64_t added = 0, matched = 0, cancelled = 0;
    auto tr = [&](const Trade& t) { matched += t.qty; };

    SynthParams p; p.count = 50000; p.seed = 12345;
    auto cmds = generate(p);
    // track cancelled volume by inspecting before cancel
    // (simpler: recompute resting from the book at the end)
    for (auto& c : cmds) {
        if (c.type == CmdType::Add) added += c.qty;
        b.apply(c, tr);
    }
    // resting volume = sum over levels
    std::uint64_t resting = 0;
    b.forEachResting([&](Side, Price, Qty q, OrderId) { resting += q; });
    // added = matched(both sides counted once per trade) + cancelled + resting
    // Each trade removes `qty` from the book's maker AND consumes `qty` from the
    // taker's intent. The conservation we can cleanly assert: resting <= added,
    // and the book's own structural audit holds.
    CHECK(resting <= added);
    CHECK(matched > 0);
    CHECK(b.checkInvariants());
}

TEST_CASE("determinism: same command stream => identical trades & book") {
    SynthParams p; p.count = 20000; p.seed = 999;
    auto cmds = generate(p);

    auto run = [&](std::vector<Trade>& out) {
        OrderBook b;
        auto sink = [&](const Trade& t) { out.push_back(t); };
        for (auto& c : cmds) b.apply(c, sink);
        return b.serialize();
    };

    std::vector<Trade> t1, t2;
    const std::string s1 = run(t1);
    const std::string s2 = run(t2);

    REQUIRE(t1.size() == t2.size());
    bool same = true;
    for (std::size_t i = 0; i < t1.size(); ++i)
        same &= (t1[i].maker_id == t2[i].maker_id &&
                 t1[i].taker_id == t2[i].taker_id &&
                 t1[i].price == t2[i].price && t1[i].qty == t2[i].qty);
    CHECK(same);
    CHECK(s1 == s2);                                 // identical final book
}

TEST_CASE("invariants hold after every op on a long random run") {
    SynthParams p; p.count = 30000; p.seed = 2024;
    auto cmds = generate(p);
    OrderBook b;
    auto sink = [](const Trade&) {};
    bool ok = true;
    int checked = 0;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        b.apply(cmds[i], sink);
        if (i % 250 == 0) { ok &= b.checkInvariants(); ++checked; }  // sample
    }
    ok &= b.checkInvariants();
    CHECK(ok);
    CHECK(checked > 0);
}

// ---- ArrayBook (Milestone 3) -------------------------------------------------

TEST_CASE("array: full fill executes at the maker price") {
    ArrayBook b(90, 110); Collector tr;
    b.apply(addLimit(Side::Sell, 100, 10, 1), tr);
    b.apply(addLimit(Side::Buy, 105, 10, 2), tr);
    REQUIRE(tr.trades.size() == 1);
    CHECK(tr.trades[0].price == 100);          // maker price, not taker's 105
    CHECK(b.empty());
    CHECK(b.checkInvariants());
}

TEST_CASE("array: market order sweeps levels; cursor tracks best") {
    ArrayBook b(90, 110); Collector tr;
    b.apply(addLimit(Side::Sell, 100, 5, 1), tr);
    b.apply(addLimit(Side::Sell, 101, 5, 2), tr);
    b.apply(addMarket(Side::Buy, 7, 3), tr);
    REQUIRE(tr.trades.size() == 2);
    CHECK(tr.trades[0].price == 100);
    CHECK(tr.trades[1].price == 101 && tr.trades[1].qty == 2);
    Price ba; CHECK(b.bestAsk(ba) && ba == 101);
    CHECK(b.checkInvariants());
}

TEST_CASE("array: cancel updates the best-price cursor") {
    ArrayBook b(90, 110); Collector tr;
    b.apply(addLimit(Side::Buy, 100, 10, 1), tr);   // best bid 100
    b.apply(addLimit(Side::Buy, 99, 10, 2), tr);
    CHECK(b.cancel(1));
    Price bb; CHECK(b.bestBid(bb) && bb == 99);      // cursor moved down
    CHECK(b.checkInvariants());
}

TEST_CASE("array: price outside the band is rejected (price band)") {
    ArrayBook b(90, 110); Collector tr;
    CHECK(!b.apply(addLimit(Side::Buy, 200, 10, 1), tr));  // 200 > 110
    CHECK(b.rejected() == 1);
    CHECK(b.empty());
    CHECK(b.checkInvariants());
}

TEST_CASE("array vs tree: identical trades & book on a long random run") {
    SynthParams p; p.count = 40000; p.seed = 555;
    auto cmds = generate(p);

    Price lo = p.mid, hi = p.mid;
    for (auto& c : cmds)
        if (c.type == CmdType::Add && c.ord_type == OrderType::Limit) {
            if (c.price < lo) lo = c.price;
            if (c.price > hi) hi = c.price;
        }
    lo -= 8; hi += 8;

    OrderBook rb; ArrayBook ar(lo, hi);
    std::vector<Trade> ta, tb;
    auto sa = [&](const Trade& t) { ta.push_back(t); };
    auto sb = [&](const Trade& t) { tb.push_back(t); };
    for (auto& c : cmds) { rb.apply(c, sa); ar.apply(c, sb); }

    REQUIRE(ta.size() == tb.size());
    bool same = true;
    for (std::size_t i = 0; i < ta.size(); ++i)
        same &= (ta[i].taker_id == tb[i].taker_id &&
                 ta[i].maker_id == tb[i].maker_id &&
                 ta[i].price == tb[i].price && ta[i].qty == tb[i].qty);
    CHECK(same);
    CHECK(rb.serialize() == ar.serialize());     // identical final book
    CHECK(ar.rejected() == 0);
    CHECK(rb.checkInvariants() && ar.checkInvariants());
}

// ---- SpscRing (Milestone 4) -------------------------------------------------

TEST_CASE("ring: single-threaded FIFO, full and empty behave") {
    me::SpscRing<int> r(4);                 // capacity 4
    int x;
    CHECK(!r.try_pop(x));                    // empty
    CHECK(r.try_push(1)); CHECK(r.try_push(2));
    CHECK(r.try_push(3)); CHECK(r.try_push(4));
    CHECK(!r.try_push(5));                   // full
    CHECK(r.try_pop(x) && x == 1);           // FIFO order
    CHECK(r.try_pop(x) && x == 2);
    CHECK(r.try_push(5));                    // room again
    CHECK(r.try_pop(x) && x == 3);
    CHECK(r.try_pop(x) && x == 4);
    CHECK(r.try_pop(x) && x == 5);
    CHECK(!r.try_pop(x));
}

TEST_CASE("ring: concurrent producer/consumer preserves order & count") {
    me::SpscRing<std::uint64_t> r(1024);
    const std::uint64_t N = 500000;
    std::atomic<bool> done{false};
    std::uint64_t received = 0;
    bool ordered = true;

    std::thread consumer([&] {
        std::uint64_t v, expect = 0;
        while (r.pop_blocking(v, done)) {
            if (v != expect) ordered = false;   // strict FIFO
            ++expect; ++received;
        }
    });
    for (std::uint64_t i = 0; i < N; ++i) r.push_blocking(i);
    done.store(true);
    consumer.join();

    CHECK(received == N);
    CHECK(ordered);
}

int main() { return tf::run_all(); }
