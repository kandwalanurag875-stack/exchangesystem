// itch_writer.h — emit valid NASDAQ ITCH 5.0 BinaryFILE bytes (Milestone 8).
//
// Why this exists: the spec wants the engine to parse a REAL exchange protocol,
// but NASDAQ's multi-GB sample files aren't something to depend on in a test.
// So we generate our own byte-accurate ITCH 5.0 stream (correct big-endian
// fields, correct framing, correct per-type lengths) — which genuinely
// exercises the binary parser — and ALSO let the user point itch_replay at a
// real NASDAQ `*.bin` sample for the same code path. Offsets here MUST match the
// reader in itch.h.
#ifndef ME_ITCH_WRITER_H
#define ME_ITCH_WRITER_H

#include <cstdint>
#include <vector>

#include "me/types.h"

namespace me {

class ItchWriter {
public:
    const std::vector<std::uint8_t>& bytes() const { return buf_; }
    std::size_t size() const { return buf_.size(); }

    void systemEvent(char code) {
        auto& m = begin(12);
        m[0] = 'S';
        putTs(m, ++clock_);
        m[11] = static_cast<std::uint8_t>(code);
        commit(m);
    }

    void stockDirectory(const char* sym8) {
        auto& m = begin(39);
        m[0] = 'R';
        putTs(m, ++clock_);
        put64(m, 11, 1);                 // locate-ish; unused by reconstruction
        for (int i = 0; i < 8; ++i) m[11 + i] = sym8[i] ? sym8[i] : ' ';
        commit(m);
    }

    void addOrder(std::uint64_t ref, Side side, std::uint32_t shares, std::int64_t price) {
        auto& m = begin(36);
        m[0] = 'A';
        putTs(m, ++clock_);
        put64(m, 11, ref);
        m[19] = (side == Side::Buy) ? 'B' : 'S';
        put32(m, 20, shares);
        for (int i = 0; i < 8; ++i) m[24 + i] = "AAPL    "[i];
        put32(m, 32, static_cast<std::uint32_t>(price));
        commit(m);
    }

    void execute(std::uint64_t ref, std::uint32_t shares) {
        auto& m = begin(31);
        m[0] = 'E';
        putTs(m, ++clock_);
        put64(m, 11, ref);
        put32(m, 19, shares);
        put64(m, 23, ++match_);
        commit(m);
    }

    void cancel(std::uint64_t ref, std::uint32_t shares) {
        auto& m = begin(23);
        m[0] = 'X';
        putTs(m, ++clock_);
        put64(m, 11, ref);
        put32(m, 19, shares);
        commit(m);
    }

    void del(std::uint64_t ref) {
        auto& m = begin(19);
        m[0] = 'D';
        putTs(m, ++clock_);
        put64(m, 11, ref);
        commit(m);
    }

    void replace(std::uint64_t old_ref, std::uint64_t new_ref,
                 std::uint32_t shares, std::int64_t price) {
        auto& m = begin(35);
        m[0] = 'U';
        putTs(m, ++clock_);
        put64(m, 11, old_ref);
        put64(m, 19, new_ref);
        put32(m, 27, shares);
        put32(m, 31, static_cast<std::uint32_t>(price));
        commit(m);
    }

private:
    // Reserve a scratch message of `len` bytes; commit() frames it into buf_.
    std::vector<std::uint8_t>& begin(std::size_t len) {
        scratch_.assign(len, 0);
        return scratch_;
    }
    void commit(std::vector<std::uint8_t>& m) {
        buf_.push_back(static_cast<std::uint8_t>((m.size() >> 8) & 0xFF));  // BE len
        buf_.push_back(static_cast<std::uint8_t>(m.size() & 0xFF));
        buf_.insert(buf_.end(), m.begin(), m.end());
    }
    static void put32(std::vector<std::uint8_t>& m, std::size_t o, std::uint32_t v) {
        m[o] = (v >> 24) & 0xFF; m[o+1] = (v >> 16) & 0xFF;
        m[o+2] = (v >> 8) & 0xFF; m[o+3] = v & 0xFF;
    }
    static void put64(std::vector<std::uint8_t>& m, std::size_t o, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) m[o + i] = (v >> (8 * (7 - i))) & 0xFF;
    }
    void putTs(std::vector<std::uint8_t>& m, std::uint64_t ts) {  // 48-bit @ off 5
        for (int i = 0; i < 6; ++i) m[5 + i] = (ts >> (8 * (5 - i))) & 0xFF;
    }

    std::vector<std::uint8_t> buf_;
    std::vector<std::uint8_t> scratch_;
    std::uint64_t clock_ = 0;
    std::uint64_t match_ = 0;
};

}  // namespace me

#endif  // ME_ITCH_WRITER_H
