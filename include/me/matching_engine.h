// matching_engine.h — Milestone 5: the durable engine. Wires the WAL + the
// OrderBook + recovery into one object whose contract is:
//
//   "Every accepted command is on durable storage, and after a crash the engine
//    replays the log (from the latest snapshot) and rebuilds the EXACT same
//    book — because matching is deterministic (Section 4.5)."
//
// Strip the trading words and this is an event-sourced, single-writer ledger:
// the same shape as etcd, Kafka, or a payment system. That is the systems story
// (Section 2).
#ifndef ME_MATCHING_ENGINE_H
#define ME_MATCHING_ENGINE_H

#include <chrono>
#include <cstdint>
#include <string>

#include "me/order_book.h"
#include "me/types.h"
#include "me/wal.h"

namespace me {

enum class Durability {
    Strict,   // fsync BEFORE apply — zero data loss, slowest (per-command fsync)
    Group,    // apply, then fsync every N commands — group commit, fast, may
              //   lose the last (<N) un-fsync'd commands on a power cut
    None,     // no fsync (rely on OS flush) — for pure-speed comparisons only
};

struct EngineConfig {
    std::string wal_path      = "engine.wal";
    std::string snapshot_path = "engine.snap";
    Durability  durability    = Durability::Strict;
    std::uint32_t group_size  = 1000;          // for Durability::Group
    std::size_t pool_capacity = 1u << 16;
};

class MatchingEngine {
public:
    explicit MatchingEngine(EngineConfig cfg)
        : cfg_(std::move(cfg)), book_(cfg_.pool_capacity) {}

    // Replay snapshot + WAL into the book. Call once at startup BEFORE submit().
    // Returns the number of commands replayed from the WAL.
    std::uint64_t recover() {
        Seq snap_seq = load_snapshot(cfg_.snapshot_path, book_);  // 0 if none
        std::uint64_t replayed = 0;
        auto sink = [](const Trade&) {};                          // trades recomputed
        Seq last = Wal::replay(cfg_.wal_path,
            [&](Seq, const Command& c) { book_.apply(c, sink); ++replayed; },
            snap_seq);
        seq_ = (last > snap_seq) ? last : snap_seq;
        wal_.open(cfg_.wal_path, /*truncate=*/false);             // resume appending
        return replayed;
    }

    // Submit a command: durably log it, then apply it. Honors the durability
    // policy. on_trade fires for each resulting execution.
    template <class OnTrade>
    bool submit(const Command& c, OnTrade&& on_trade) {
        // Lazily open the log so a fresh engine can submit without recover()
        // (e.g. a brand-new instance starting an empty book).
        if (!wal_.is_open()) wal_.open(cfg_.wal_path, /*truncate=*/false);
        const Seq s = seq_ + 1;
        wal_.append(c, s, now_nanos());
        if (cfg_.durability == Durability::Strict) {
            wal_.flush();                       // durable BEFORE we apply/ack
            const bool ok = book_.apply(c, on_trade);
            seq_ = s;
            return ok;
        }
        // Group / None: apply, then fsync on the configured cadence.
        const bool ok = book_.apply(c, on_trade);
        seq_ = s;
        if (cfg_.durability == Durability::Group &&
            cfg_.group_size && (s % cfg_.group_size == 0)) {
            wal_.flush();
        }
        return ok;
    }

    // Force any buffered WAL records to disk (call before a clean shutdown).
    void flush() { wal_.flush(); }

    // Write a snapshot of the current book and COMPACT the log: everything up to
    // seq_ is now captured by the durable snapshot, so the WAL is truncated and
    // future commands start a fresh, short log. The snapshot is fsync'd and
    // atomically renamed into place BEFORE the WAL is truncated, so a crash in
    // between never loses committed state.
    bool snapshot() {
        if (!write_snapshot(cfg_.snapshot_path, book_, seq_)) return false;
        wal_.open(cfg_.wal_path, /*truncate=*/true);
        return true;
    }

    OrderBook& book() { return book_; }
    const OrderBook& book() const { return book_; }
    Seq seq() const { return seq_; }

private:
    static std::uint64_t now_nanos() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    EngineConfig cfg_;
    OrderBook book_;
    Wal wal_;
    Seq seq_ = 0;
};

}  // namespace me

#endif  // ME_MATCHING_ENGINE_H
