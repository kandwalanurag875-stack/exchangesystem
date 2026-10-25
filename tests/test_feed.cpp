// test_feed.cpp — Milestone 8 correctness: ITCH 5.0 binary parsing + book
// reconstruction, and LOBSTER message parsing + cross-check helper.
#include <string>
#include <vector>

#include "me/itch.h"
#include "me/itch_writer.h"
#include "me/lobster.h"
#include "me/test_framework.h"

using namespace me;

TEST_CASE("ITCH: big-endian add orders rebuild the book at the right prices") {
    ItchWriter w;
    w.systemEvent('O');
    w.stockDirectory("AAPL");
    w.addOrder(1, Side::Buy, 50, 100000);
    w.addOrder(2, Side::Sell, 40, 100001);
    w.addOrder(3, Side::Buy, 30, 99999);

    ItchBookBuilder b;
    std::uint64_t n = itch_parse_buffer(w.bytes().data(), w.size(), b);
    REQUIRE(n == 5);                       // 2 admin + 3 adds
    CHECK(b.stats().adds == 3);
    auto& book = b.book();
    Price bb, ba;
    CHECK(book.bestBid(bb) && bb == 100000);   // big-endian price decoded right
    CHECK(book.bestAsk(ba) && ba == 100001);
    CHECK(book.restingCount() == 3);
    CHECK(book.checkInvariants());
}

TEST_CASE("ITCH: execute / cancel / delete / replace reconstruct exactly") {
    ItchWriter w;
    w.addOrder(1, Side::Buy, 50, 100000);
    w.addOrder(2, Side::Buy, 30, 99999);
    w.addOrder(3, Side::Sell, 40, 100001);
    w.execute(1, 20);                      // ref1: 50 -> 30
    w.cancel(2, 10);                       // ref2: 30 -> 20
    w.del(3);                              // ref3 gone (no asks left)
    w.replace(1, 4, 15, 100002);           // ref1(30)@100000 -> ref4(15)@100002

    ItchBookBuilder b;
    itch_parse_buffer(w.bytes().data(), w.size(), b);
    auto& book = b.book();

    // Expected resting: bid 100002 x15 (ref4), bid 99999 x20 (ref2). No asks.
    CHECK(book.restingCount() == 2);
    Price bb;
    CHECK(book.bestBid(bb) && bb == 100002);
    Price ba;
    CHECK(!book.bestAsk(ba));               // all asks deleted
    std::vector<std::pair<Price, Qty>> bids;
    book.topBids(5, bids);
    REQUIRE(bids.size() == 2);
    CHECK(bids[0].first == 100002 && bids[0].second == 15);
    CHECK(bids[1].first == 99999 && bids[1].second == 20);
    CHECK(b.stats().executes == 1 && b.stats().cancels == 1 &&
          b.stats().deletes == 1 && b.stats().replaces == 1);
    CHECK(book.checkInvariants());
}

TEST_CASE("ITCH: a truncated tail is parsed safely, no crash") {
    ItchWriter w;
    w.addOrder(1, Side::Buy, 50, 100000);
    w.addOrder(2, Side::Sell, 40, 100001);
    std::vector<std::uint8_t> bytes(w.bytes());
    bytes.resize(bytes.size() - 3);        // chop the last message mid-body

    ItchBookBuilder b;
    std::uint64_t n = itch_parse_buffer(bytes.data(), bytes.size(), b);
    CHECK(n == 1);                         // only the first full message applied
    CHECK(b.book().restingCount() == 1);
    CHECK(b.book().checkInvariants());
}

TEST_CASE("ITCH: --limit style max_msgs stops early") {
    ItchWriter w;
    for (int i = 1; i <= 10; ++i) w.addOrder(i, Side::Buy, 10, 100000 - i);
    ItchBookBuilder b;
    std::uint64_t n = itch_parse_buffer(w.bytes().data(), w.size(), b, /*max=*/4);
    CHECK(n == 4);
    CHECK(b.book().restingCount() == 4);
}

TEST_CASE("LOBSTER: message parsing + reconstruction") {
    const char* lines[] = {
        "0.1,1,1,50,100000,1",     // add buy  id1 50 @100000
        "0.2,1,2,40,100001,-1",    // add sell id2 40 @100001
        "0.3,4,1,20,100000,1",     // execute id1 20 -> 30 left
        "0.4,3,2,0,100001,-1",     // delete id2
    };
    LobsterBook lb;
    for (auto* l : lines) {
        LobMsg m;
        REQUIRE(parse_lobster_line(l, m));
        lb.apply(m);
    }
    auto& book = lb.book();
    CHECK(book.restingCount() == 1);
    Price bb, ba;
    CHECK(book.bestBid(bb) && bb == 100000);
    CHECK(!book.bestAsk(ba));               // sell deleted
    std::vector<std::pair<Price, Qty>> bids;
    book.topBids(5, bids);
    REQUIRE(bids.size() == 1);
    CHECK(bids[0].second == 30);            // 50 - 20 executed
    CHECK(book.checkInvariants());
}

TEST_CASE("LOBSTER: cross-check helper matches a constructed orderbook row") {
    LobsterBook lb;
    LobMsg m;
    parse_lobster_line("0,1,1,50,100000,1", m); lb.apply(m);   // bid 100000 x50
    parse_lobster_line("0,1,2,40,100001,-1", m); lb.apply(m);  // ask 100001 x40

    // orderbook row (level 1): ask_px, ask_sz, bid_px, bid_sz
    std::vector<std::string> row = {"100001", "40", "100000", "50"};
    CHECK(top_levels_match(lb.book(), row, 1));

    std::vector<std::string> bad = {"100001", "999", "100000", "50"};
    CHECK(!top_levels_match(lb.book(), bad, 1));
}

TEST_CASE("LOBSTER L2: per-step apply reproduces the next snapshot") {
    // Real LOBSTER-style check: seed from a snapshot, apply one message, and the
    // predicted top levels must equal the next snapshot. Also proves l2_match
    // actually detects mismatches (not trivially true).
    L2Book book;
    book.add(Side::Sell, 100001, 40);
    book.add(Side::Sell, 100002, 30);
    book.add(Side::Buy, 100000, 50);
    book.add(Side::Buy, 99999, 25);

    // message: execute 10 of the best bid (id irrelevant for L2)
    LobMsg m; m.type = 4; m.size = 10; m.price = 100000; m.dir = 1;
    L2Book pred = book; l2_apply(pred, m);

    L2Book expected;                         // best bid now 100000 x40
    expected.add(Side::Sell, 100001, 40);
    expected.add(Side::Sell, 100002, 30);
    expected.add(Side::Buy, 100000, 40);
    expected.add(Side::Buy, 99999, 25);
    CHECK(l2_match(pred, expected, 5));
    CHECK(!l2_match(book, expected, 5));     // pre-apply book must NOT match

    // a full delete (type 3) removes the level
    LobMsg d; d.type = 3; d.size = 40; d.price = 100000; d.dir = 1;
    l2_apply(pred, d);
    L2Book after;
    after.add(Side::Sell, 100001, 40);
    after.add(Side::Sell, 100002, 30);
    after.add(Side::Buy, 99999, 25);         // 100000 gone, best bid now 99999
    CHECK(l2_match(pred, after, 5));
}

TEST_CASE("LOBSTER L2: parse an orderbook row into levels") {
    // row layout: ask_px1, ask_sz1, bid_px1, bid_sz1, ask_px2, ...
    std::vector<std::string> row = {"100001","40","100000","50","100002","30","99999","25"};
    L2Book b; parse_orderbook_row(row, 2, b);
    REQUIRE(b.asks.begin()->first == 100001);
    CHECK(b.asks.begin()->second == 40);
    CHECK(b.bids.begin()->first == 100000);
    CHECK(b.bids.begin()->second == 50);
}

int main() { return tf::run_all(); }
