// recover_demo.cpp — Milestone 5 demo: "kill it mid-run, it recovers exactly."
//
//   recover_demo selftest [--orders N]   # run + simulated crash + recover, in
//                                         # one process; asserts exact recovery
//   recover_demo run      [--orders N]   # process N orders durably, print the
//                                         # pre-crash book digest, then "crash"
//                                         # (exit without clean shutdown)
//   recover_demo recover                 # rebuild from WAL+snapshot, print digest
//
// For the live two-process demo: run `... run`, note the digest, then `...
// recover` in a second invocation and confirm the digest matches.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "me/matching_engine.h"
#include "me/synthetic.h"
#include "me/wal.h"

using namespace me;

static const char* kWal  = "engine.wal";
static const char* kSnap = "engine.snap";

// 64-bit FNV digest of the canonical book serialization — a one-number fingerprint.
static std::uint64_t book_digest(const OrderBook& b) {
    const std::string s = b.serialize();
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s) { h ^= static_cast<std::uint8_t>(c); h *= 1099511628211ull; }
    return h;
}

static std::vector<Command> workload(std::size_t n) {
    SynthParams p; p.count = n; p.seed = 0xBEEF; return generate(p);
}

static int do_run(std::size_t n, bool snapshot_midway) {
    std::remove(kWal); std::remove(kSnap);            // fresh start
    EngineConfig cfg; cfg.wal_path = kWal; cfg.snapshot_path = kSnap;
    cfg.durability = Durability::Strict;              // every command fsync'd
    MatchingEngine eng(cfg);

    auto cmds = workload(n);
    std::uint64_t trades = 0;
    auto sink = [&](const Trade&) { ++trades; };
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        eng.submit(cmds[i], sink);
        if (snapshot_midway && i + 1 == cmds.size() / 2) eng.snapshot();
    }
    std::printf("RUN: submitted %zu commands, %llu trades\n", cmds.size(),
                (unsigned long long)trades);
    std::printf("RUN: seq=%llu  resting=%zu  book_digest=%016llx\n",
                (unsigned long long)eng.seq(), eng.book().restingCount(),
                (unsigned long long)book_digest(eng.book()));
    std::printf("RUN: --- simulating crash (exit without clean shutdown) ---\n");
    // NOTE: we deliberately do NOT call a clean shutdown. In Strict mode every
    // command was already fsync'd, so the WAL on disk is complete.
    return 0;
}

static int do_recover() {
    EngineConfig cfg; cfg.wal_path = kWal; cfg.snapshot_path = kSnap;
    MatchingEngine eng(cfg);
    const std::uint64_t replayed = eng.recover();
    std::printf("RECOVER: replayed %llu WAL records\n", (unsigned long long)replayed);
    std::printf("RECOVER: seq=%llu  resting=%zu  book_digest=%016llx\n",
                (unsigned long long)eng.seq(), eng.book().restingCount(),
                (unsigned long long)book_digest(eng.book()));
    std::printf("RECOVER: invariants=%s\n",
                eng.book().checkInvariants() ? "PASS" : "*** FAIL ***");
    return 0;
}

static int do_selftest(std::size_t n) {
    std::remove(kWal); std::remove(kSnap);
    EngineConfig cfg; cfg.wal_path = kWal; cfg.snapshot_path = kSnap;
    cfg.durability = Durability::Strict;

    std::uint64_t digest_before, seq_before, resting_before;
    {
        MatchingEngine eng(cfg);
        auto cmds = workload(n);
        auto sink = [](const Trade&) {};
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            eng.submit(cmds[i], sink);
            if (i + 1 == cmds.size() / 2) eng.snapshot();   // exercise snapshot+compaction
        }
        digest_before = book_digest(eng.book());
        seq_before = eng.seq();
        resting_before = eng.book().restingCount();
        std::printf("BEFORE crash: seq=%llu resting=%llu digest=%016llx\n",
                    (unsigned long long)seq_before, (unsigned long long)resting_before,
                    (unsigned long long)digest_before);
        // eng goes out of scope WITHOUT a clean shutdown == simulated crash
    }

    MatchingEngine eng2(cfg);
    const std::uint64_t replayed = eng2.recover();
    const std::uint64_t digest_after = book_digest(eng2.book());
    std::printf("AFTER  recover: seq=%llu resting=%zu digest=%016llx (replayed %llu)\n",
                (unsigned long long)eng2.seq(), eng2.book().restingCount(),
                (unsigned long long)digest_after, (unsigned long long)replayed);

    const bool ok = (digest_after == digest_before) &&
                    (eng2.seq() == seq_before) &&
                    (eng2.book().restingCount() == resting_before) &&
                    eng2.book().checkInvariants();
    std::printf("\nRECOVERY %s — recovered book is %s to pre-crash book.\n",
                ok ? "PASS" : "*** FAIL ***",
                ok ? "BYTE-IDENTICAL" : "DIFFERENT");
    return ok ? 0 : 2;
}

int main(int argc, char** argv) {
    std::string cmd = (argc > 1) ? argv[1] : "selftest";
    std::size_t n = 200000;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--orders") && i + 1 < argc)
            n = std::strtoull(argv[++i], nullptr, 10);

    if (cmd == "run")      return do_run(n, /*snapshot_midway=*/true);
    if (cmd == "recover")  return do_recover();
    return do_selftest(n);
}
