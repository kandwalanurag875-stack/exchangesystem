// types.h — the shared vocabulary of the engine: orders, sides, commands,
// trades. Kept dependency-free so every component can include it cheaply.
#ifndef ME_TYPES_H
#define ME_TYPES_H

#include <cstdint>
#include <limits>

namespace me {

using OrderId = std::uint64_t;
using Seq     = std::uint64_t;   // monotonic command / trade sequence number

// Prices are integer ticks, NOT floating point. Floats break determinism
// (rounding is platform-dependent) and equality on a price level must be exact.
// A real venue quotes in whole ticks anyway. int64 leaves head-room for the
// +/-infinity sentinels we use to model market orders.
using Price = std::int64_t;

// Quantities are non-negative but we keep them signed-width-safe by using a
// 64-bit unsigned type; the matching loop never lets a qty go below zero.
using Qty = std::uint64_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : std::uint8_t {
    Limit  = 0,   // rest the unmatched remainder in the book
    Market = 1,   // never rests; cancels any unmatched remainder
};

// Time-in-force flags (IOC/FOK are Section 4.3 stretch items, wired here so the
// matching core can honour them without a later type change).
enum class Tif : std::uint8_t {
    Gtc = 0,   // Good-til-cancelled (default for limit orders)
    Ioc = 1,   // Immediate-or-cancel: match what you can, drop the rest
    Fok = 2,   // Fill-or-kill: fully fill or do nothing
};

enum class CmdType : std::uint8_t {
    Add    = 0,
    Cancel = 1,
    Modify = 2,
};

// Market orders are modelled as limit orders priced at +/-infinity so the SAME
// matching code handles both (Section 4.3 trick). A market buy will cross any
// ask; a market sell will cross any bid.
constexpr Price kMarketBuyPrice  = std::numeric_limits<Price>::max();
constexpr Price kMarketSellPrice = std::numeric_limits<Price>::min();

// A command is the unit of input AND the unit of durability: exactly this
// struct is what gets written to the WAL (Milestone 5). Replaying the same
// command stream must reproduce the same book + trades (Section 4.5).
struct Command {
    CmdType   type     = CmdType::Add;
    Side      side     = Side::Buy;
    OrderType ord_type = OrderType::Limit;
    Tif       tif      = Tif::Gtc;
    OrderId   id       = 0;
    Price     price    = 0;
    Qty       qty      = 0;
};

// A trade (execution). Always priced at the RESTING (maker) order's price — the
// order already in the book sets the price (Section 6.3). Getting this wrong is
// a classic interview trap (Section 15).
struct Trade {
    Seq     seq        = 0;   // global execution sequence
    OrderId taker_id   = 0;   // the incoming/aggressing order
    OrderId maker_id   = 0;   // the resting order that was hit
    Side    taker_side = Side::Buy;
    Price   price      = 0;   // = maker's resting price
    Qty     qty        = 0;
};

}  // namespace me

#endif  // ME_TYPES_H
