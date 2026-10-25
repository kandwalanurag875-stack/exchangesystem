// order_book.h — the matching core (Layer 1, Milestone 1).
//
// Data structure (Section 6.1), the interview-classic layering:
//   * price levels  -> std::map  (a Red-Black tree). Sorted iteration gives the
//                      best price at begin(); O(log P) insert/find, P = number
//                      of distinct price levels. Bids use std::greater so
//                      begin() is the HIGHEST bid; asks use the default less so
//                      begin() is the LOWEST ask. Both: "best == begin()".
//   * orders @level  -> intrusive doubly-linked list (FIFO). Append at tail,
//                      match from head => price-TIME priority. DLL gives O(1)
//                      unlink from the middle, which is what makes cancel cheap.
//   * id -> node     -> std::unordered_map<OrderId, Order*> so cancel is O(1):
//                      jump straight to the node and unlink it.
//
// The std::map-vs-flat-array tradeoff (Section 6.1) is the subject of the
// Milestone 3 benchmark; see include/me/array_book.h for the alternative.
#ifndef ME_ORDER_BOOK_H
#define ME_ORDER_BOOK_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "me/object_pool.h"
#include "me/platform.h"
#include "me/types.h"

namespace me {

// A resting order living inside a price level's FIFO list. Intrusive links keep
// it pointer-stable (it lives in the ObjectPool, never in a vector that resizes)
// so the index map can hold a raw Order* safely.
struct Order {
    OrderId id    = 0;
    Side    side  = Side::Buy;
    Price   price = 0;
    Qty     qty   = 0;          // remaining (unfilled) quantity
    Order*  prev  = nullptr;    // older order at this level (toward head)
    Order*  next  = nullptr;    // newer order at this level (toward tail)
    void*   level = nullptr;    // back-ptr to PriceLevel for O(1) cleanup
};

struct PriceLevel {
    Price  price     = 0;
    Qty    total_qty = 0;       // sum of resting qty (cheap depth for the UI)
    Order* head      = nullptr; // oldest order (matches first)
    Order* tail      = nullptr; // newest order (appended last)
};

class OrderBook {
public:
    explicit OrderBook(std::size_t pool_capacity = 1u << 16)
        : pool_(pool_capacity) {
        index_.reserve(pool_capacity);
    }

    // -- main entry point: apply one command -------------------------------
    // on_trade is invoked once per execution. Templated (not std::function) so
    // it inlines to nothing on the hot path.
    template <class OnTrade>
    bool apply(const Command& c, OnTrade&& on_trade) {
        switch (c.type) {
            case CmdType::Add:
                if (c.ord_type == OrderType::Market)
                    return addMarket(c.side, c.qty, c.id, c.tif, on_trade);
                return addLimit(c.side, c.price, c.qty, c.id, c.tif, on_trade);
            case CmdType::Cancel:
                return cancel(c.id);
            case CmdType::Modify:
                return modify(c.id, c.price, c.qty, on_trade);
        }
        return false;
    }

    // -- limit order -------------------------------------------------------
    template <class OnTrade>
    bool addLimit(Side side, Price price, Qty qty, OrderId id, Tif tif,
                  OnTrade&& on_trade) {
        if (qty == 0) return false;
        if (index_.count(id)) return false;  // id already resting => reject

        // Fill-or-kill: must be fully fillable up-front or we do nothing at all.
        if (tif == Tif::Fok && !fullyFillable(side, price, qty)) return false;

        Qty remaining = (side == Side::Buy)
            ? matchAgainst(asks_, price, qty, id, side, /*buy=*/true, on_trade)
            : matchAgainst(bids_, price, qty, id, side, /*buy=*/false, on_trade);

        if (remaining > 0 && tif == Tif::Gtc) {
            // Rest the unmatched remainder; market/IOC never rest.
            if (side == Side::Buy) restInto(bids_, side, price, remaining, id);
            else                   restInto(asks_, side, price, remaining, id);
        }
        return true;
    }

    // -- market order (modelled as a limit at +/-infinity, never rests) ----
    template <class OnTrade>
    bool addMarket(Side side, Qty qty, OrderId id, Tif /*tif*/,
                   OnTrade&& on_trade) {
        if (qty == 0) return false;
        const Price p = (side == Side::Buy) ? kMarketBuyPrice : kMarketSellPrice;
        if (side == Side::Buy)
            matchAgainst(asks_, p, qty, id, side, /*buy=*/true, on_trade);
        else
            matchAgainst(bids_, p, qty, id, side, /*buy=*/false, on_trade);
        return true;  // remainder (if any) is dropped — market orders never rest
    }

    // -- cancel: O(1) find + unlink ----------------------------------------
    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        Order* o = it->second;
        auto* lvl = static_cast<PriceLevel*>(o->level);
        unlink(lvl, o);
        lvl->total_qty -= o->qty;
        const Side side = o->side;
        const Price price = o->price;
        index_.erase(it);
        pool_.deallocate(o);
        if (lvl->head == nullptr) {  // level emptied -> drop it (O(log P))
            if (side == Side::Buy) bids_.erase(price);
            else                   asks_.erase(price);
        }
        return true;
    }

    // -- reduce a resting order's quantity (used by feed reconstruction in
    //    Milestone 8: an ITCH/LOBSTER execution or partial cancel shrinks a
    //    resting order without re-matching). Removes the order if it hits 0.
    bool reduce(OrderId id, Qty by) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        Order* o = it->second;
        auto* lvl = static_cast<PriceLevel*>(o->level);
        const Qty d = (by < o->qty) ? by : o->qty;
        o->qty        -= d;
        lvl->total_qty -= d;
        if (o->qty == 0) {
            unlink(lvl, o);
            const Side side = o->side;
            const Price price = o->price;
            index_.erase(it);
            pool_.deallocate(o);
            if (lvl->head == nullptr) {
                if (side == Side::Buy) bids_.erase(price);
                else                   asks_.erase(price);
            }
        }
        return true;
    }

    // Read a resting order's side/price/qty by id (Milestone 8: ITCH "replace"
    // needs the original order's side). Returns false if not resting.
    bool orderInfo(OrderId id, Side& side, Price& price, Qty& qty) const {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        side = it->second->side; price = it->second->price; qty = it->second->qty;
        return true;
    }

    // -- modify == cancel + re-add. A price/size change re-queues at the tail,
    //    losing time priority (correct exchange behaviour on a price change,
    //    Section 6.2 / Section 15). We pick the simple, well-defined rule and
    //    document it rather than the size-keeps-priority special case.
    template <class OnTrade>
    bool modify(OrderId id, Price new_price, Qty new_qty, OnTrade&& on_trade) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        const Side side = it->second->side;
        cancel(id);
        if (new_qty == 0) return true;
        return addLimit(side, new_price, new_qty, id, Tif::Gtc, on_trade);
    }

    // -- queries -----------------------------------------------------------
    bool bestBid(Price& out) const {
        if (bids_.empty()) return false;
        out = bids_.begin()->first;
        return true;
    }
    bool bestAsk(Price& out) const {
        if (asks_.empty()) return false;
        out = asks_.begin()->first;
        return true;
    }
    bool empty() const { return bids_.empty() && asks_.empty(); }
    std::size_t restingCount() const { return index_.size(); }

    // Top-N levels for the visualizer (Section 10). Cheap snapshot, no mutation.
    void topBids(std::size_t n, std::vector<std::pair<Price, Qty>>& out) const {
        out.clear();
        for (auto it = bids_.begin(); it != bids_.end() && out.size() < n; ++it)
            out.emplace_back(it->first, it->second.total_qty);
    }
    void topAsks(std::size_t n, std::vector<std::pair<Price, Qty>>& out) const {
        out.clear();
        for (auto it = asks_.begin(); it != asks_.end() && out.size() < n; ++it)
            out.emplace_back(it->first, it->second.total_qty);
    }

    // -- durability helpers (Milestone 5) ----------------------------------
    // Visit every resting order in canonical order (bids desc, then asks asc,
    // each level head->tail). Used to write snapshots deterministically.
    template <class Fn>
    void forEachResting(Fn&& fn) const {
        for (auto& kv : bids_)
            for (Order* o = kv.second.head; o; o = o->next)
                fn(Side::Buy, o->price, o->qty, o->id);
        for (auto& kv : asks_)
            for (Order* o = kv.second.head; o; o = o->next)
                fn(Side::Sell, o->price, o->qty, o->id);
    }
    // Re-insert a resting order during snapshot load (no matching performed —
    // a snapshot is by definition an uncrossed book).
    void loadResting(Side side, Price price, Qty qty, OrderId id) {
        if (side == Side::Buy) restInto(bids_, side, price, qty, id);
        else                   restInto(asks_, side, price, qty, id);
    }

    // -- testing aids (Section 6.4 / Section 11) ---------------------------
    // Canonical, deterministic serialization. Two books with identical resting
    // state produce byte-identical strings => powers determinism & recovery
    // tests without exposing internals.
    std::string serialize() const {
        std::ostringstream os;
        os << "BIDS\n";
        for (auto& kv : bids_) dumpLevel(os, kv.second);
        os << "ASKS\n";
        for (auto& kv : asks_) dumpLevel(os, kv.second);
        return os.str();
    }

    // Full structural audit (Section 6.4). Returns true iff every invariant
    // holds; tests call this after each operation.
    bool checkInvariants() const {
        std::size_t counted = 0;
        Price prev_bid; bool have_prev = false;
        for (auto& kv : bids_) {
            if (kv.first != kv.second.price) return false;
            if (have_prev && !(prev_bid > kv.first)) return false;  // strictly desc
            prev_bid = kv.first; have_prev = true;
            if (!auditLevel(kv.second, Side::Buy, counted)) return false;
        }
        have_prev = false; Price prev_ask;
        for (auto& kv : asks_) {
            if (kv.first != kv.second.price) return false;
            if (have_prev && !(prev_ask < kv.first)) return false;  // strictly asc
            prev_ask = kv.first; have_prev = true;
            if (!auditLevel(kv.second, Side::Sell, counted)) return false;
        }
        if (counted != index_.size()) return false;          // index <-> levels
        Price bb, ba;                                          // never crossed
        if (bestBid(bb) && bestAsk(ba) && !(bb < ba)) return false;
        return true;
    }

    Seq tradeSeq() const { return trade_seq_; }

private:
    // Bids: highest price first. Asks: lowest price first. Both => best at begin.
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    // The generic FIFO matcher, shared by both sides. `buy` selects the crossing
    // test: a buy crosses an ask when ask_price <= limit; a sell crosses a bid
    // when bid_price >= limit. Trades print at the maker's (level) price.
    template <class OppMap, class OnTrade>
    Qty matchAgainst(OppMap& opp, Price limit, Qty qty, OrderId taker_id,
                     Side taker_side, bool buy, OnTrade&& on_trade) {
        while (qty > 0 && !opp.empty()) {
            auto it = opp.begin();
            PriceLevel& lvl = it->second;
            const bool crosses = buy ? (lvl.price <= limit) : (lvl.price >= limit);
            if (!crosses) break;

            while (qty > 0 && lvl.head) {
                Order* maker = lvl.head;
                const Qty traded = std::min(qty, maker->qty);
                on_trade(Trade{++trade_seq_, taker_id, maker->id, taker_side,
                               lvl.price, traded});
                qty            -= traded;
                maker->qty     -= traded;
                lvl.total_qty  -= traded;
                if (maker->qty == 0) {              // maker fully consumed
                    lvl.head = maker->next;
                    if (lvl.head) lvl.head->prev = nullptr;
                    else          lvl.tail = nullptr;
                    index_.erase(maker->id);
                    pool_.deallocate(maker);
                }
            }
            if (lvl.head == nullptr) opp.erase(it);  // level fully consumed
        }
        return qty;
    }

    // FOK pre-check: is there at least `qty` available at crossing levels?
    bool fullyFillable(Side side, Price limit, Qty qty) const {
        Qty avail = 0;
        if (side == Side::Buy) {
            for (auto& kv : asks_) {
                if (kv.first > limit) break;
                avail += kv.second.total_qty;
                if (avail >= qty) return true;
            }
        } else {
            for (auto& kv : bids_) {
                if (kv.first < limit) break;
                avail += kv.second.total_qty;
                if (avail >= qty) return true;
            }
        }
        return avail >= qty;
    }

    template <class Map>
    void restInto(Map& book, Side side, Price price, Qty qty, OrderId id) {
        Order* o = pool_.allocate();
        o->id = id; o->side = side; o->price = price; o->qty = qty;
        o->prev = o->next = nullptr;

        auto it = book.find(price);
        if (it == book.end()) {
            PriceLevel lvl;
            lvl.price = price;
            it = book.emplace(price, lvl).first;
        }
        PriceLevel& lvl = it->second;
        o->level = &lvl;
        o->prev = lvl.tail;                  // append at tail (FIFO)
        if (lvl.tail) lvl.tail->next = o;
        else          lvl.head = o;
        lvl.tail = o;
        lvl.total_qty += qty;
        index_[id] = o;
    }

    static void unlink(PriceLevel* lvl, Order* o) {
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
        if (lvl.head == nullptr) return false;       // no empty levels allowed
        Qty sum = 0; Order* prev = nullptr;
        for (Order* o = lvl.head; o; o = o->next) {
            if (o->side != side) return false;
            if (o->price != lvl.price) return false;
            if (o->level != &lvl) return false;
            if (o->prev != prev) return false;        // DLL backward link
            auto fit = index_.find(o->id);
            if (fit == index_.end() || fit->second != o) return false;
            sum += o->qty;
            prev = o;
            ++counted;
        }
        if (lvl.tail != prev) return false;
        return sum == lvl.total_qty;
    }

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, Order*> index_;
    ObjectPool<Order> pool_;
    Seq trade_seq_ = 0;
};

}  // namespace me

#endif  // ME_ORDER_BOOK_H
