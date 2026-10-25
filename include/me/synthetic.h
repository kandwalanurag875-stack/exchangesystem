// synthetic.h — generate realistic synthetic order flow for benchmarking.
//
// Distribution follows the spec (Section 3 / Section 9): prices ~ Normal around
// a mid, a realistic mix of ~add/cancel with a slice of executing orders.
// Defaults: ~78% add-limit, ~14% cancel, ~8% market — the same shape used in
// the CoinTossX paper and the reference repos.
//
// IMPORTANT: the randomness here is in INPUT GENERATION only, seeded and
// reproducible. It never enters the matching path, so determinism (Section 4.5)
// is preserved: same seed => same command stream => same trades.
#ifndef ME_SYNTHETIC_H
#define ME_SYNTHETIC_H

#include <cstdint>
#include <random>
#include <vector>

#include "me/types.h"

namespace me {

struct SynthParams {
    std::size_t count    = 1'000'000;
    Price       mid      = 10'000;   // mid price in ticks
    double      sigma    = 50.0;     // stddev of price around mid (ticks)
    Qty         max_qty  = 100;
    double      p_cancel = 0.14;     // fraction cancels
    double      p_market = 0.08;     // fraction market orders
    std::uint64_t seed   = 0xC0FFEE; // reproducible
};

inline std::vector<Command> generate(const SynthParams& p) {
    std::vector<Command> out;
    out.reserve(p.count);

    std::mt19937_64 rng(p.seed);
    std::normal_distribution<double> price_dist(static_cast<double>(p.mid), p.sigma);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<std::uint64_t> qty_dist(1, p.max_qty);

    std::vector<OrderId> live;        // resting order ids eligible for cancel
    live.reserve(p.count / 2);
    OrderId next_id = 1;

    for (std::size_t i = 0; i < p.count; ++i) {
        const double roll = unit(rng);
        Command c{};

        if (roll < p.p_cancel && !live.empty()) {
            // cancel a random live order (swap-remove from the live set)
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t idx = pick(rng);
            c.type = CmdType::Cancel;
            c.id   = live[idx];
            live[idx] = live.back();
            live.pop_back();
        } else if (roll < p.p_cancel + p.p_market) {
            // marketable order — guaranteed to consume liquidity if any rests
            c.type     = CmdType::Add;
            c.ord_type = OrderType::Market;
            c.side     = (unit(rng) < 0.5) ? Side::Buy : Side::Sell;
            c.qty      = qty_dist(rng);
            c.id       = next_id++;
        } else {
            // resting / crossing limit order
            c.type     = CmdType::Add;
            c.ord_type = OrderType::Limit;
            c.side     = (unit(rng) < 0.5) ? Side::Buy : Side::Sell;
            c.price    = static_cast<Price>(price_dist(rng));
            c.qty      = qty_dist(rng);
            c.id       = next_id;
            live.push_back(next_id);
            ++next_id;
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace me

#endif  // ME_SYNTHETIC_H
