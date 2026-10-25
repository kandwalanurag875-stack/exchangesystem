// wal.h — Milestone 5: the write-ahead log + snapshots (the systems story).
//
// Discipline (Section 8.1), the same one databases and the student's DynamicKV
// use: a command is appended to an on-disk log and fsync'd BEFORE the engine
// acknowledges/applies it. Writes are append-only and sequential (fast); disk
// is never random-accessed on the hot path.
//
// Each record carries a magic + a CRC32 so a torn/partial write (a crash in the
// middle of a write) is DETECTED on recovery: replay stops at the last fully
// valid record. That is what makes "kill it mid-run and it recovers exactly"
// true rather than hopeful.
//
// Format assumption: x86-64 little-endian (both the Windows dev box and the
// Linux target). The record is a #pragma-packed POD written verbatim.
#ifndef ME_WAL_H
#define ME_WAL_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "me/platform.h"
#include "me/types.h"

namespace me {

// ---- CRC32 (IEEE 0xEDB88320), table built once -----------------------------
inline std::uint32_t crc32(const void* data, std::size_t n) {
    static std::uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) c = tbl[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

#pragma pack(push, 1)
struct WalRecord {
    std::uint32_t magic;     // kWalMagic — resync / sanity
    std::uint64_t seq;       // monotonic sequence number
    std::uint8_t  type;      // CmdType
    std::uint8_t  side;      // Side
    std::uint8_t  ord_type;  // OrderType
    std::uint8_t  tif;       // Tif
    std::uint64_t id;
    std::int64_t  price;
    std::uint64_t qty;
    std::uint64_t ts_nanos;  // wall-clock at append; AUDIT ONLY — never feeds
                             // matching, so determinism (Sec 4.5) is preserved.
    std::uint32_t crc;       // crc32 over all preceding bytes of this record
};
#pragma pack(pop)

constexpr std::uint32_t kWalMagic  = 0x314C4157;  // "WAL1"
constexpr std::uint32_t kSnapMagic = 0x31504E53;  // "SNP1"

inline std::uint32_t record_crc(const WalRecord& r) {
    return crc32(&r, sizeof(WalRecord) - sizeof(std::uint32_t));
}

// ---- the append-only log ----------------------------------------------------
class Wal {
public:
    ~Wal() { close(); }

    // Open for append (creating if needed). truncate=true starts a fresh log.
    bool open(const std::string& path, bool truncate = false) {
        close();
        path_ = path;
        f_ = std::fopen(path.c_str(), truncate ? "wb" : "ab");
        return f_ != nullptr;
    }
    void close() {
        if (f_) { std::fflush(f_); std::fclose(f_); f_ = nullptr; }
    }
    bool is_open() const { return f_ != nullptr; }

    // Append one record. Does NOT force to disk — call flush() for that.
    void append(const Command& c, Seq seq, std::uint64_t ts_nanos) {
        WalRecord r{};
        r.magic = kWalMagic;
        r.seq = seq;
        r.type = static_cast<std::uint8_t>(c.type);
        r.side = static_cast<std::uint8_t>(c.side);
        r.ord_type = static_cast<std::uint8_t>(c.ord_type);
        r.tif = static_cast<std::uint8_t>(c.tif);
        r.id = c.id; r.price = c.price; r.qty = c.qty;
        r.ts_nanos = ts_nanos;
        r.crc = record_crc(r);
        std::fwrite(&r, sizeof(r), 1, f_);
    }

    // fflush (userspace) + fsync (OS cache -> physical storage). After this
    // returns, the appended records survive a power loss.
    bool flush() { return f_ ? durable_flush(f_) == 0 : false; }

    // Replay every valid record with seq > after_seq, calling
    // fn(seq, Command). Stops at the first bad/torn record (returns last good
    // seq). Safe to call on a missing file (returns after_seq).
    template <class Fn>
    static Seq replay(const std::string& path, Fn&& fn, Seq after_seq = 0) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return after_seq;
        Seq last = after_seq;
        WalRecord r{};
        while (std::fread(&r, sizeof(r), 1, f) == 1) {
            if (r.magic != kWalMagic) break;             // garbage / wrong file
            if (r.crc != record_crc(r)) break;           // torn write -> stop
            if (r.seq > after_seq) {
                Command c{};
                c.type = static_cast<CmdType>(r.type);
                c.side = static_cast<Side>(r.side);
                c.ord_type = static_cast<OrderType>(r.ord_type);
                c.tif = static_cast<Tif>(r.tif);
                c.id = r.id; c.price = r.price; c.qty = r.qty;
                fn(r.seq, c);
                last = r.seq;
            }
        }
        std::fclose(f);
        return last;
    }

private:
    std::FILE* f_ = nullptr;
    std::string path_;
};

// ---- snapshots (Section 8.3: log compaction) --------------------------------
// A snapshot is the full resting book at a given seq. On recovery: load the
// snapshot, then replay only the WAL records AFTER its seq — so a huge log
// doesn't have to be replayed from zero (same idea as Raft log compaction /
// LSM checkpoints).
#pragma pack(push, 1)
struct SnapHeader {
    std::uint32_t magic;
    std::uint64_t seq;       // the book equals state after applying up to here
    std::uint64_t count;     // number of resting-order entries that follow
};
struct SnapEntry {
    std::uint8_t  side;
    std::int64_t  price;
    std::uint64_t qty;
    std::uint64_t id;
};
#pragma pack(pop)

// Book must expose forEachResting(fn(Side,Price,Qty,OrderId)).
template <class Book>
inline bool write_snapshot(const std::string& path, const Book& book, Seq seq) {
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    SnapHeader h{kSnapMagic, seq, book.restingCount()};
    std::fwrite(&h, sizeof(h), 1, f);
    std::uint32_t crc = crc32(&h, sizeof(h));
    book.forEachResting([&](Side s, Price p, Qty q, OrderId id) {
        SnapEntry e{static_cast<std::uint8_t>(s), p, q, id};
        std::fwrite(&e, sizeof(e), 1, f);
        crc ^= crc32(&e, sizeof(e));
    });
    std::fwrite(&crc, sizeof(crc), 1, f);
    const bool ok = (durable_flush(f) == 0);
    std::fclose(f);
    if (!ok) return false;
    // atomic-ish swap: rename tmp over the real path (snapshot is all-or-nothing)
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

// Load a snapshot into a fresh book via book.loadResting(...). Returns the
// snapshot seq (0 if no/invalid snapshot — caller then replays the whole WAL).
template <class Book>
inline Seq load_snapshot(const std::string& path, Book& book) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    SnapHeader h{};
    if (std::fread(&h, sizeof(h), 1, f) != 1 || h.magic != kSnapMagic) {
        std::fclose(f); return 0;
    }
    std::uint32_t crc = crc32(&h, sizeof(h));
    bool ok = true;
    for (std::uint64_t i = 0; i < h.count; ++i) {
        SnapEntry e{};
        if (std::fread(&e, sizeof(e), 1, f) != 1) { ok = false; break; }
        crc ^= crc32(&e, sizeof(e));
        book.loadResting(static_cast<Side>(e.side), e.price, e.qty, e.id);
    }
    std::uint32_t file_crc = 0;
    if (ok && std::fread(&file_crc, sizeof(file_crc), 1, f) == 1 && file_crc == crc) {
        std::fclose(f);
        return h.seq;
    }
    std::fclose(f);
    return 0;  // corrupt snapshot -> ignore, fall back to full WAL replay
}

}  // namespace me

#endif  // ME_WAL_H
