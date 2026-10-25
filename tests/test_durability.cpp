// test_durability.cpp — Milestone 5 recovery tests (Section 11).
// Crash recovery is the systems story's load-bearing claim, so it gets its own
// suite: clean recovery, snapshot+tail replay, torn-write tolerance, and CRC
// corruption detection.
#include <cstdio>
#include <string>
#include <vector>

#include "me/matching_engine.h"
#include "me/order_book.h"
#include "me/synthetic.h"
#include "me/test_framework.h"
#include "me/wal.h"

using namespace me;

static const char* WAL  = "tmp_durability.wal";
static const char* SNAP = "tmp_durability.snap";

static void cleanup() { std::remove(WAL); std::remove(SNAP); }

static std::vector<Command> workload(std::size_t n, std::uint64_t seed) {
    SynthParams p; p.count = n; p.seed = seed; return generate(p);
}

// Build a reference book by applying commands directly (no WAL).
static std::string reference_book(const std::vector<Command>& cmds, std::size_t upto) {
    OrderBook b;
    auto sink = [](const Trade&) {};
    for (std::size_t i = 0; i < upto && i < cmds.size(); ++i) b.apply(cmds[i], sink);
    return b.serialize();
}

TEST_CASE("recovery: WAL replay rebuilds an identical book (no snapshot)") {
    cleanup();
    auto cmds = workload(20000, 1);
    std::string before;
    {
        EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
        cfg.durability = Durability::Strict;
        MatchingEngine eng(cfg);
        auto sink = [](const Trade&) {};
        for (auto& c : cmds) eng.submit(c, sink);
        before = eng.book().serialize();
        // no clean shutdown == crash; Strict already fsync'd every record
    }
    EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
    MatchingEngine eng2(cfg);
    std::uint64_t replayed = eng2.recover();
    CHECK(replayed == cmds.size());
    CHECK(eng2.book().serialize() == before);
    CHECK(eng2.book().serialize() == reference_book(cmds, cmds.size()));
    CHECK(eng2.book().checkInvariants());
    cleanup();
}

TEST_CASE("recovery: snapshot + WAL tail rebuilds an identical book") {
    cleanup();
    auto cmds = workload(20000, 2);
    std::string before;
    {
        EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
        cfg.durability = Durability::Strict;
        MatchingEngine eng(cfg);
        auto sink = [](const Trade&) {};
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            eng.submit(cmds[i], sink);
            if (i + 1 == cmds.size() / 2) eng.snapshot();   // compaction point
        }
        before = eng.book().serialize();
    }
    EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
    MatchingEngine eng2(cfg);
    eng2.recover();                                          // snapshot + tail
    CHECK(eng2.book().serialize() == before);
    CHECK(eng2.seq() == cmds.size());
    CHECK(eng2.book().checkInvariants());
    cleanup();
}

TEST_CASE("recovery: determinism — recovered trades match a fresh replay") {
    cleanup();
    auto cmds = workload(15000, 3);
    {
        EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
        cfg.durability = Durability::Strict;
        MatchingEngine eng(cfg);
        auto sink = [](const Trade&) {};
        for (auto& c : cmds) eng.submit(c, sink);
    }
    // Replaying the WAL and re-running matching must produce the same trades.
    std::uint64_t replay_trades = 0, replay_vol = 0;
    OrderBook b;
    auto sink = [&](const Trade& t) { ++replay_trades; replay_vol += t.qty; };
    Wal::replay(WAL, [&](Seq, const Command& c) { b.apply(c, sink); });

    std::uint64_t direct_trades = 0, direct_vol = 0;
    OrderBook b2;
    auto sink2 = [&](const Trade& t) { ++direct_trades; direct_vol += t.qty; };
    for (auto& c : cmds) b2.apply(c, sink2);

    CHECK(replay_trades == direct_trades);
    CHECK(replay_vol == direct_vol);
    CHECK(b.serialize() == b2.serialize());
    cleanup();
}

TEST_CASE("torn write: a partial trailing record is ignored, book stays consistent") {
    cleanup();
    auto cmds = workload(5000, 4);
    {
        EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
        cfg.durability = Durability::Strict;
        MatchingEngine eng(cfg);
        auto sink = [](const Trade&) {};
        for (auto& c : cmds) eng.submit(c, sink);
    }
    // Simulate a crash mid-write: append a few garbage bytes (a torn record).
    {
        std::FILE* f = std::fopen(WAL, "ab");
        REQUIRE(f != nullptr);
        const char junk[17] = "XXXXXXXXXXXXXXXX";
        std::fwrite(junk, 1, sizeof(junk), f);
        std::fclose(f);
    }
    EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
    MatchingEngine eng2(cfg);
    std::uint64_t replayed = eng2.recover();
    CHECK(replayed == cmds.size());                       // torn tail skipped
    CHECK(eng2.book().serialize() == reference_book(cmds, cmds.size()));
    CHECK(eng2.book().checkInvariants());
    cleanup();
}

TEST_CASE("corruption: a flipped bit truncates replay at the bad record") {
    cleanup();
    const std::size_t N = 200;
    auto cmds = workload(N, 5);
    {
        EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
        cfg.durability = Durability::Strict;
        MatchingEngine eng(cfg);
        auto sink = [](const Trade&) {};
        for (auto& c : cmds) eng.submit(c, sink);
    }
    // Flip a byte inside record index 9 (the 10th record => seq 10).
    {
        std::FILE* f = std::fopen(WAL, "r+b");
        REQUIRE(f != nullptr);
        const long off = static_cast<long>(9 * sizeof(WalRecord) + 12);
        std::fseek(f, off, SEEK_SET);
        int byte = std::fgetc(f);
        std::fseek(f, off, SEEK_SET);
        std::fputc(byte ^ 0x5A, f);                       // corrupt
        std::fclose(f);
    }
    EngineConfig cfg; cfg.wal_path = WAL; cfg.snapshot_path = SNAP;
    MatchingEngine eng2(cfg);
    eng2.recover();
    // CRC catches it: replay stops before the corrupted record (seq 10),
    // so the recovered state equals applying the first 9 commands.
    CHECK(eng2.seq() == 9);
    CHECK(eng2.book().serialize() == reference_book(cmds, 9));
    CHECK(eng2.book().checkInvariants());
    cleanup();
}

int main() { return tf::run_all(); }
