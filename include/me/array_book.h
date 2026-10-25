// array_book.h — Milestone 3: the flat-array price-level alternative.
//
// Same matching semantics as OrderBook, different price-level container:
//   * Levels live in a FLAT, pre-sized std::vector indexed directly by price
//     tick (idx = price - base). O(1) access to ANY level, contiguous memory,
//     cache-friendly — no pointer-chasing through RB-tree nodes, no per-level
//     malloc on first touch.
//   * Best-price access is O(1) amortised via two cursors (best_bid_/best_ask_)
//     that we nudge as levels fill and empty.
//
// The TRADEOFF (Section 6.1) we want to MEASURE against OrderBook:
//   + faster, predictable, allocation-free level access (great cache locality)
//   - requires a bounded, known price range fixed up front
//   - wastes memory across empty ticks in a sparse/wide book
//   - a price outside [min,max] is rejected (acts like an exchange price band)
//
// Same DLL-of-orders + ObjectPool + unordered_map(id->node) as OrderBook, so
// cancel stays O(1). Vectors are sized once and never resized => PriceLevel
// addresses are stable, so Order::level back-pointers stay valid.
#ifndef ME_ARRAY_BOOK_H
#define ME_ARRAY_BOOK_H

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "me/object_pool.h"
#include "me/order_book.h"  // reuse Order / PriceLevel
#include "me/types.h"

namespace me {

class ArrayBook {
public:
    ArrayBook(Price min_price, Price max_price, std::size_t pool_capacity = 1u << 16)
        : base_(min_price),
          range_(static_cast<long>(max_price - min_price + 1)),
          bids_(static_cast<std::size_t>(range_)),
          asks_(static_cast<std::size_t>(range_)),
          pool_(pool_capacity),
          best_ask_(range_) {
        index_.reserve(pool_capacity);
        // pre-stamp each slot's price so audits/serialisation are trivial
        for (long i = 0; i < range_; ++i) {
            bids_[i].price = base_ + i;
            asks_[i].price = base_ + i;
        }
    }

    template <class OnTrade>
    bool apply(const Command& c, OnTrade&& on_trade) {
        switch (c.type) {
            case CmdType::Add:
                if (c.ord_type == OrderType::Market)
                    return addMarket(c.side, c.qty, c.id, on_trade);
                return addLimit(c.side, c.price, c.qty, c.id, c.tif, on_trade);
            case CmdType::Cancel: return cancel(c.id);
            case CmdType::Modify: return modify(c.id, c.price, c.qty, on_trade);
        }
        return false;
    }

    template <class OnTrade>
    bool addLimit(Side side, Price price, Qty qty, OrderId id, Tif tif,
                  OnTrade&& on_trade) {
        if (qty == 0) return false;
        if (index_.count(id)) return false;
        const long idx = static_cast<long>(price - base_);
        if (idx < 0 || idx >= range_) { ++rejected_; return false; }  // price band
        if (tif == Tif::Fok && !fullyFillable(side, idx, qty)) return false;

        Qty rem;
        if (side == Side::Buy) rem = consumeAsks(idx, qty, id, on_trade);
        else                   rem = consumeBids(idx, qty, id, on_trade);

        if (rem > 0 && tif == Tif::Gtc) {
            if (side == Side::Buy) restBid(idx, rem, id);
            else                   restAsk(idx, rem, id);
        }
        return true;
    }

    template <class OnTrade>
    bool addMarket(Side side, Qty qty, OrderId id, OnTrade&& on_trade) {
        if (qty == 0) return false;
        if (side == Side::Buy) consumeAsks(range_ - 1, qty, id, on_trade); // any ask
        else                   consumeBids(0, qty, id, on_trade);          // any bid
        return true;  // never rests
    }

    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        Order* o = it->second;
        auto* lvl = static_cast<PriceLevel*>(o->level);
        unlinkNode(lvl, o);
        lvl->total_qty -= o->qty;
        const Side side = o->side;
        const long idx = static_cast<long>(o->price - base_);
        index_.erase(it);
        pool_.deallocate(o);
        if (lvl->head == nullptr) {                 // level emptied
            if (side == Side::Buy) { if (idx == best_bid_) advanceBestBid(); }
            else                   { if (idx == best_ask_) advanceBestAsk(); }
        }
        return true;
    }

    template <class OnTrade>
    bool modify(OrderId id, Price new_price, Qty new_qty, OnTrade&& on_trade) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Side side = it->second->side;
        cancel(id);
        if (new_qty == 0) return true;
        return addLimit(side, new_price, new_qty, id, Tif::Gtc, on_trade);
    }

    bool bestBid(Price& out) const {
        if (best_bid_ < 0) return false;
        out = base_ + best_bid_; return true;
    }
    bool bestAsk(Price& out) const {
        if (best_ask_ >= range_) return false;
        out = base_ + best_ask_; return true;
    }
    bool empty() const { return index_.empty(); }
    std::size_t restingCount() const { return index_.size(); }
    std::size_t rejected() const { return rejected_; }

    void topBids(std::size_t n, std::vector<std::pair<Price, Qty>>& out) const {
        out.clear();
        for (long i = best_bid_; i >= 0 && out.size() < n; --i)
            if (bids_[i].head) out.emplace_back(bids_[i].price, bids_[i].total_qty);
    }
    void topAsks(std::size_t n, std::vector<std::pair<Price, Qty>>& out) const {
        out.clear();
        for (long i = best_ask_; i < range_ && out.size() < n; ++i)
            if (asks_[i].head) out.emplace_back(asks_[i].price, asks_[i].total_qty);
    }

    // Canonical order identical to OrderBook (bids desc, asks asc) so the two
    // implementations can be cross-checked byte-for-byte.
    template <class Fn>
    void forEachResting(Fn&& fn) const {
        for (long i = range_ - 1; i >= 0; --i)
            for (Order* o = bids_[i].head; o; o = o->next)
                fn(Side::Buy, o->price, o->qty, o->id);
        for (long i = 0; i < range_; ++i)
            for (Order* o = asks_[i].head; o; o = o->next)
                fn(Side::Sell, o->price, o->qty, o->id);
    }

    std::string serialize() const {
        std::ostringstream os;
        os << "BIDS\n";
        for (long i = range_ - 1; i >= 0; --i)
            if (bids_[i].head) dumpLevel(os, bids_[i]);
        os << "ASKS\n";
        for (long i = 0; i < range_; ++i)
            if (asks_[i].head) dumpLevel(os, asks_[i]);
        return os.str();
    }

    bool checkInvariants() const {
        std::size_t counted = 0;
        long top_bid = -1, low_ask = range_;
        for (long i = 0; i < range_; ++i) {
            if (bids_[i].head) {
                if (bids_[i].price != base_ + i) return false;
                if (!auditLevel(bids_[i], Side::Buy, counted)) return false;
                if (i > top_bid) top_bid = i;
            }
            if (asks_[i].head) {
                if (asks_[i].price != base_ + i) return false;
                if (!auditLevel(asks_[i], Side::Sell, counted)) return false;
                if (i < low_ask) low_ask = i;
            }
        }
        if (top_bid != best_bid_) return false;       // cursor accuracy
        if (low_ask != best_ask_) return false;
        if (counted != index_.size()) return false;
        if (top_bid >= 0 && low_ask < range_ && !(top_bid < low_ask)) return false;
        return true;
    }

    Seq tradeSeq() const { return trade_seq_; }

private:
    template <class OnTrade>
    Qty consumeAsks(long idx_limit, Qty qty, OrderId taker, OnTrade&& on_trade) {
        while (qty > 0 && best_ask_ < range_ && best_ask_ <= idx_limit) {
            PriceLevel& lvl = asks_[best_ask_];
            while (qty > 0 && lvl.head) {
                Order* maker = lvl.head;
                const Qty traded = qty < maker->qty ? qty : maker->qty;
                on_trade(Trade{++trade_seq_, taker, maker->id, Side::Buy,
                               lvl.price, traded});
                qty -= traded; maker->qty -= traded; lvl.total_qty -= traded;
                if (maker->qty == 0) {
                    lvl.head = maker->next;
                    if (lvl.head) lvl.head->prev = nullptr; else lvl.tail = nullptr;
                    index_.erase(maker->id);
                    pool_.deallocate(maker);
                }
            }
            if (lvl.head == nullptr) advanceBestAsk();
        }
        return qty;
    }

    template <class OnTrade>
    Qty consumeBids(long idx_limit, Qty qty, OrderId taker, OnTrade&& on_trade) {
        while (qty > 0 && best_bid_ >= 0 && best_bid_ >= idx_limit) {
            PriceLevel& lvl = bids_[best_bid_];
            while (qty > 0 && lvl.head) {
                Order* maker = lvl.head;
                const Qty traded = qty < maker->qty ? qty : maker->qty;
                on_trade(Trade{++trade_seq_, taker, maker->id, Side::Sell,
                               lvl.price, traded});
                qty -= traded; maker->qty -= traded; lvl.total_qty -= traded;
                if (maker->qty == 0) {
                    lvl.head = maker->next;
                    if (lvl.head) lvl.head->prev = nullptr; else lvl.tail = nullptr;
                    index_.erase(maker->id);
                    pool_.deallocate(maker);
                }
            }
            if (lvl.head == nullptr) advanceBestBid();
        }
        return qty;
    }

    void restBid(long idx, Qty qty, OrderId id) {
        appendNode(bids_[idx], Side::Buy, base_ + idx, qty, id);
        if (idx > best_bid_) best_bid_ = idx;
    }
    void restAsk(long idx, Qty qty, OrderId id) {
        appendNode(asks_[idx], Side::Sell, base_ + idx, qty, id);
        if (best_ask_ >= range_ || idx < best_ask_) best_ask_ = idx;
    }

    void appendNode(PriceLevel& lvl, Side side, Price price, Qty qty, OrderId id) {
        Order* o = pool_.allocate();
        o->id = id; o->side = side; o->price = price; o->qty = qty;
        o->prev = lvl.tail; o->next = nullptr; o->level = &lvl;
        if (lvl.tail) lvl.tail->next = o; else lvl.head = o;
        lvl.tail = o;
        lvl.total_qty += qty;
        index_[id] = o;
    }

    bool fullyFillable(Side side, long idx, Qty qty) const {
        Qty avail = 0;
        if (side == Side::Buy) {
            for (long i = best_ask_; i < range_ && i <= idx; ++i)
                if (asks_[i].head) { avail += asks_[i].total_qty; if (avail >= qty) return true; }
        } else {
            for (long i = best_bid_; i >= 0 && i >= idx; --i)
                if (bids_[i].head) { avail += bids_[i].total_qty; if (avail >= qty) return true; }
        }
        return avail >= qty;
    }

    void advanceBestAsk() { while (best_ask_ < range_ && asks_[best_ask_].head == nullptr) ++best_ask_; }
    void advanceBestBid() { while (best_bid_ >= 0 && bids_[best_bid_].head == nullptr) --best_bid_; }

    static void unlinkNode(PriceLevel* lvl, Order* o) {
        if (o->prev) o->prev->next = o->next; else lvl->head = o->next;
        if (o->next) o->next->prev = o->prev; else lvl->tail = o->prev;
    }
    static void dumpLevel(std::ostringstream& os, const PriceLevel& lvl) {
        os << lvl.price << ':';
        for (Order* o = lvl.head; o; o = o->next)
            os << o->id << 'x' << o->qty << (o->next ? "," : "");
        os << '\n';
    }
    bool auditLevel(const PriceLevel& lvl, Side side, std::size_t& counted) const {
        Qty sum = 0; Order* prev = nullptr;
        for (Order* o = lvl.head; o; o = o->next) {
            if (o->side != side || o->price != lvl.price || o->level != &lvl) return false;
            if (o->prev != prev) return false;
            auto fit = index_.find(o->id);
            if (fit == index_.end() || fit->second != o) return false;
            sum += o->qty; prev = o; ++counted;
        }
        return lvl.tail == prev && sum == lvl.total_qty;
    }

    Price base_;
    long  range_;
    std::vector<PriceLevel> bids_;   // index = price - base_
    std::vector<PriceLevel> asks_;
    std::unordered_map<OrderId, Order*> index_;
    ObjectPool<Order> pool_;
    long best_bid_ = -1;             // highest occupied bid idx (-1 = none)
    long best_ask_;                  // lowest occupied ask idx (range_ = none)
    Seq trade_seq_ = 0;
    std::size_t rejected_ = 0;
};

}  // namespace me

#endif  // ME_ARRAY_BOOK_H
