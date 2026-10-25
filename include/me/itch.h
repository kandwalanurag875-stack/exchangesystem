// itch.h — Milestone 8: a NASDAQ TotalView-ITCH 5.0 feed parser + book
// reconstructor (the most HFT-specific skill in the spec, Section 12).
//
// ITCH is the real wire protocol a US equity exchange uses to broadcast every
// order-book event. It is a BINARY, big-endian, order-by-order feed. We parse
// the "BinaryFILE" framing NASDAQ uses for its downloadable sample files: each
// message is preceded by a 2-byte big-endian length, and the first byte of the
// message body is the message type.
//
// Reconstruction (not matching!): an ITCH feed already tells you exactly what
// happened, so we APPLY events to the book directly rather than re-running the
// matcher:
//     A / F  add order      -> rest the order (no matching)
//     E      order executed -> reduce that order by the executed shares
//     C      executed w/ px  -> reduce that order by the executed shares
//     X      order cancel    -> reduce that order by the cancelled shares
//     D      order delete    -> remove that order entirely
//     U      order replace   -> delete old ref, add new ref (px/qty changed)
//     P / Q  trade messages  -> counted, no displayed-book change
//
// Prices in ITCH 5.0 are 4-byte unsigned fixed-point with 4 implied decimals
// (price = raw / 10000). We keep the raw integer ticks — exactly what the
// engine wants (integer prices, Section 4.5 determinism).
#ifndef ME_ITCH_H
#define ME_ITCH_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "me/order_book.h"
#include "me/types.h"

namespace me {

// ---- big-endian readers (ITCH is network byte order) ------------------------
inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
inline std::uint64_t be48(const std::uint8_t* p) {  // 48-bit timestamp
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}
inline std::uint64_t be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// Per-type body length (bytes, INCLUDING the 1-byte type tag) for ITCH 5.0.
// Used to skip messages we don't reconstruct from. 0 => unknown (use framing).
inline std::size_t itch_msg_len(char type) {
    switch (type) {
        case 'S': return 12;   // system event
        case 'R': return 39;   // stock directory
        case 'H': return 25;   // stock trading action
        case 'Y': return 20;   // reg SHO
        case 'L': return 26;   // market participant position
        case 'V': return 35;   // MWCB decline level
        case 'W': return 12;   // MWCB status
        case 'K': return 28;   // IPO quoting period
        case 'A': return 36;   // add order (no MPID)
        case 'F': return 40;   // add order (with MPID)
        case 'E': return 31;   // order executed
        case 'C': return 36;   // order executed with price
        case 'X': return 23;   // order cancel
        case 'D': return 19;   // order delete
        case 'U': return 35;   // order replace
        case 'P': return 44;   // trade (non-cross)
        case 'Q': return 40;   // cross trade
        case 'B': return 19;   // broken trade
        case 'I': return 50;   // NOII / imbalance
        default:  return 0;
    }
}

struct ItchStats {
    std::uint64_t total = 0, adds = 0, executes = 0, cancels = 0,
                  deletes = 0, replaces = 0, trades = 0, skipped = 0;
};

// Reconstructs an order book from an ITCH 5.0 byte stream. The order reference
// number (8 bytes) is used directly as our OrderId.
class ItchBookBuilder {
public:
    explicit ItchBookBuilder(std::size_t pool = 1u << 20) : book_(pool) {}

    // Apply one already-located message body (len includes the type tag).
    // Returns false only on a malformed (too-short) message.
    bool apply(const std::uint8_t* m, std::size_t len) {
        if (len < 1) return false;
        const char t = static_cast<char>(m[0]);
        ++stats_.total;
        switch (t) {
            case 'A': return onAdd(m, len, /*mpid=*/false);
            case 'F': return onAdd(m, len, /*mpid=*/true);
            case 'E': return onExecute(m, len, /*with_price=*/false);
            case 'C': return onExecute(m, len, /*with_price=*/true);
            case 'X': return onCancel(m, len);
            case 'D': return onDelete(m, len);
            case 'U': return onReplace(m, len);
            case 'P': case 'Q': ++stats_.trades; return true;
            default:  ++stats_.skipped; return true;   // not book-affecting
        }
    }

    OrderBook& book() { return book_; }
    const ItchStats& stats() const { return stats_; }

private:
    // 'A': locate(2) track(2) ts(6) ref(8) side(1) shares(4) stock(8) price(4)
    //  offsets from start of message (byte 0 = type 'A'):
    //   type@0 locate@1 track@3 ts@5 ref@11 side@19 shares@20 stock@24 price@32
    bool onAdd(const std::uint8_t* m, std::size_t len, bool mpid) {
        const std::size_t need = mpid ? 40 : 36;
        if (len < need) return false;
        const std::uint64_t ref = be64(m + 11);
        const Side side = (m[19] == 'B') ? Side::Buy : Side::Sell;
        const Qty shares = be32(m + 20);
        const Price price = static_cast<Price>(be32(m + 32));
        book_.loadResting(side, price, shares, ref);   // rest, do not match
        ++stats_.adds;
        return true;
    }

    // 'E': type@0 locate@1 track@3 ts@5 ref@11 execShares@19 matchNum@23
    // 'C': ... execShares@19 matchNum@23 printable@31 execPrice@32
    bool onExecute(const std::uint8_t* m, std::size_t len, bool with_price) {
        if (len < (with_price ? 36u : 31u)) return false;
        const std::uint64_t ref = be64(m + 11);
        const Qty exec = be32(m + 19);
        book_.reduce(ref, exec);
        ++stats_.executes;
        return true;
    }

    // 'X': type@0 locate@1 track@3 ts@5 ref@11 cancelledShares@19
    bool onCancel(const std::uint8_t* m, std::size_t len) {
        if (len < 23) return false;
        const std::uint64_t ref = be64(m + 11);
        const Qty cancelled = be32(m + 19);
        book_.reduce(ref, cancelled);
        ++stats_.cancels;
        return true;
    }

    // 'D': type@0 locate@1 track@3 ts@5 ref@11
    bool onDelete(const std::uint8_t* m, std::size_t len) {
        if (len < 19) return false;
        const std::uint64_t ref = be64(m + 11);
        book_.cancel(ref);
        ++stats_.deletes;
        return true;
    }

    // 'U': type@0 locate@1 track@3 ts@5 oldRef@11 newRef@19 shares@27 price@31
    bool onReplace(const std::uint8_t* m, std::size_t len) {
        if (len < 35) return false;
        const std::uint64_t old_ref = be64(m + 11);
        const std::uint64_t new_ref = be64(m + 19);
        const Qty shares = be32(m + 27);
        const Price price = static_cast<Price>(be32(m + 31));
        Side side = Side::Buy; Price op; Qty oq;
        if (book_.orderInfo(old_ref, side, op, oq)) {
            book_.cancel(old_ref);
            book_.loadResting(side, price, shares, new_ref);
        }
        ++stats_.replaces;
        return true;
    }

    OrderBook book_;
    ItchStats stats_;
};

// Parse an entire ITCH 5.0 BinaryFILE buffer (2-byte BE length framing),
// feeding each message to `builder`. Returns messages processed. Stops cleanly
// at a truncated tail. `max_msgs`==0 means no limit.
inline std::uint64_t itch_parse_buffer(const std::uint8_t* data, std::size_t n,
                                       ItchBookBuilder& builder,
                                       std::uint64_t max_msgs = 0) {
    std::uint64_t processed = 0;
    std::size_t off = 0;
    while (off + 2 <= n) {
        const std::uint16_t mlen = be16(data + off);
        if (mlen == 0 || off + 2 + mlen > n) break;     // truncated / padding
        builder.apply(data + off + 2, mlen);
        off += 2 + mlen;
        if (++processed == max_msgs && max_msgs) break;
    }
    return processed;
}

// Convenience: read a whole ITCH file into memory and parse it.
inline std::uint64_t itch_parse_file(const std::string& path,
                                     ItchBookBuilder& builder,
                                     std::uint64_t max_msgs = 0) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return 0; }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(sz));
    std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return itch_parse_buffer(buf.data(), got, builder, max_msgs);
}

}  // namespace me

#endif  // ME_ITCH_H
