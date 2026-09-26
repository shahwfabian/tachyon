#pragma once

// Tachyon: a single-instrument, price-time-priority limit order book.
//
// Design (see README for the rationale):
//   * Prices are integer ticks in [0, num_ticks). One flat array of price levels is shared by
//     both sides; an uncrossed book can never have bids and asks at the same tick.
//   * Orders live in a preallocated pool and are chained into per-level FIFO queues through
//     32-bit intrusive links. No allocation on the hot path.
//   * One bitmap per side marks non-empty levels. The best price is cached; when a best level
//     empties, the next one is found with countr_zero / countl_zero over 64-bit words.
//   * Order ids are dense exchange-assigned integers, so id -> slot lookup is a single index.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include "types.hpp"

#ifdef TACHYON_PLANTED_BUGS
#define TACHYON_BUG(b) ((bugs_ & (b)) != 0)
#else
#define TACHYON_BUG(b) false
#endif

namespace tachyon {

struct BookConfig {
    Price num_ticks = 1 << 16;        // price grid size
    std::uint32_t max_order_id = 1u << 24;  // ids must be < this
    std::uint32_t max_resting = 1u << 20;   // pool capacity
};

class OrderBook {
public:
    static constexpr std::uint32_t kNil = 0xFFFFFFFFu;

    explicit OrderBook(const BookConfig& cfg = {})
        : num_ticks_(cfg.num_ticks),
          pool_(cfg.max_resting),
          index_(cfg.max_order_id, kNil),
          levels_(static_cast<std::size_t>(cfg.num_ticks)),
          bid_bits_((static_cast<std::size_t>(cfg.num_ticks) + 63) / 64, 0),
          ask_bits_((static_cast<std::size_t>(cfg.num_ticks) + 63) / 64, 0) {
        for (std::uint32_t i = 0; i < cfg.max_resting; ++i) pool_[i].next = i + 1 < cfg.max_resting ? i + 1 : kNil;
        free_head_ = cfg.max_resting ? 0 : kNil;
        best_bid_ = -1;
        best_ask_ = num_ticks_;
    }

    // ---- order entry -------------------------------------------------------------------

    // Limit order: matches against the opposite side up to `price`, rests any remainder.
    template <class Sink>
    Status add(OrderId id, Side side, Price price, Qty qty, Sink&& sink) {
        if (qty == 0 || price < 0 || price >= num_ticks_ || id >= index_.size() || index_[id] != kNil)
            return Status::Rejected;
        qty = match(id, side, price, qty, sink);
        if (qty != 0) {
            if (free_head_ == kNil) return Status::Rejected;  // pool exhausted; remainder dropped
            rest(id, side, price, qty);
        }
        return Status::Ok;
    }

    // Market order: immediate-or-cancel against any price. Never rests.
    template <class Sink>
    Status market(OrderId id, Side side, Qty qty, Sink&& sink) {
        if (qty == 0 || id >= index_.size() || index_[id] != kNil) return Status::Rejected;
        const Price limit = side == Side::Buy ? num_ticks_ - 1 : 0;
        Price last = -1;
        struct Tracking {
            Sink& inner;
            Price& last;
            void on_fill(const Fill& f) { last = f.price; inner.on_fill(f); }
        } track{sink, last};
        const Qty left = match(id, side, limit, qty, track);
        if (TACHYON_BUG(kBugMarketRemainderRests) && left != 0 && last >= 0 && free_head_ != kNil)
            rest(id, side, last, left);
        return Status::Ok;
    }

    Status cancel(OrderId id) {
        if (id >= index_.size() || index_[id] == kNil) return Status::Rejected;
        remove(index_[id], /*from_cancel=*/true);
        return Status::Ok;
    }

    // Modify price and/or quantity. A same-price size reduction keeps time priority;
    // anything else is a cancel-replace that loses priority and may trade.
    template <class Sink>
    Status modify(OrderId id, Price price, Qty qty, Sink&& sink) {
        if (id >= index_.size() || index_[id] == kNil || price < 0 || price >= num_ticks_)
            return Status::Rejected;
        if (qty == 0) return cancel(id);
        const std::uint32_t s = index_[id];
        Order& o = pool_[s];
        const bool same_price = o.price == price;
        if (same_price && (qty <= o.qty || TACHYON_BUG(kBugModifyUpKeepsPriority))) {
            Level& lv = levels_[o.price];
            lv.qty = lv.qty - o.qty + qty;
            o.qty = qty;
            return Status::Ok;
        }
        const Side side = o.side;
        remove(s, /*from_cancel=*/false);
        return add(id, side, price, qty, sink);
    }

    // ---- queries -----------------------------------------------------------------------

    bool has_bid() const noexcept { return best_bid_ >= 0; }
    bool has_ask() const noexcept { return best_ask_ < num_ticks_; }
    Price best_bid() const noexcept { return best_bid_; }
    Price best_ask() const noexcept { return best_ask_; }
    Price num_ticks() const noexcept { return num_ticks_; }
    std::uint32_t resting_orders() const noexcept { return resting_; }
    std::uint64_t level_qty(Price p) const noexcept { return levels_[p].qty; }

    // Walk non-empty levels best-to-worst: f(price, total_qty, order_count).
    template <class F>
    void for_each_level(Side side, F&& f) const {
        if (side == Side::Buy) {
            for (Price p = scan_down_raw(bid_bits_, num_ticks_ - 1); p >= 0; p = scan_down_raw(bid_bits_, p - 1))
                f(p, levels_[p].qty, levels_[p].count);
        } else {
            for (Price p = scan_up_raw(ask_bits_, 0); p < num_ticks_; p = scan_up_raw(ask_bits_, p + 1))
                f(p, levels_[p].qty, levels_[p].count);
        }
    }

    // Walk the FIFO queue at a price: f(id, qty).
    template <class F>
    void for_each_order_at(Price p, F&& f) const {
        for (std::uint32_t s = levels_[p].head; s != kNil; s = pool_[s].next) f(pool_[s].id, pool_[s].qty);
    }

    // Full structural self-check. O(ticks/64 + orders). Used by tests and the fuzzer.
    bool check_invariants(std::string* why) const {
        auto fail = [&](std::string msg) {
            if (why) *why = std::move(msg);
            return false;
        };
        const Price real_bid = scan_down_raw(bid_bits_, num_ticks_ - 1);
        const Price real_ask = scan_up_raw(ask_bits_, 0);
        if (real_bid != best_bid_)
            return fail("cached best bid " + std::to_string(best_bid_) + " != bitmap best bid " + std::to_string(real_bid));
        if (real_ask != best_ask_)
            return fail("cached best ask " + std::to_string(best_ask_) + " != bitmap best ask " + std::to_string(real_ask));
        if (has_bid() && has_ask() && best_bid_ >= best_ask_)
            return fail("crossed book: bid " + std::to_string(best_bid_) + " >= ask " + std::to_string(best_ask_));
        std::uint32_t seen = 0;
        for (Price p = 0; p < num_ticks_; ++p) {
            const Level& lv = levels_[p];
            const bool b = bit(bid_bits_, p), a = bit(ask_bits_, p);
            if (b && a) return fail("level " + std::to_string(p) + " flagged on both sides");
            if ((b || a) != (lv.count != 0))
                return fail("level " + std::to_string(p) + " bitmap/count mismatch");
            std::uint64_t sum = 0;
            std::uint32_t n = 0, prev = kNil;
            for (std::uint32_t s = lv.head; s != kNil; s = pool_[s].next) {
                const Order& o = pool_[s];
                if (o.prev != prev) return fail("broken prev link at level " + std::to_string(p));
                if (o.price != p) return fail("order " + std::to_string(o.id) + " queued at wrong level");
                if (o.qty == 0) return fail("zero-quantity order " + std::to_string(o.id) + " resting");
                if ((o.side == Side::Buy) != b) return fail("order " + std::to_string(o.id) + " on wrong side bitmap");
                if (index_[o.id] != s) return fail("index mismatch for order " + std::to_string(o.id));
                sum += o.qty;
                ++n;
                prev = s;
                if (n > resting_) return fail("cycle in level " + std::to_string(p));
            }
            if (lv.tail != prev) return fail("bad tail at level " + std::to_string(p));
            if (n != lv.count) return fail("level " + std::to_string(p) + " count mismatch");
            if (n != 0 && sum != lv.qty)
                return fail("level " + std::to_string(p) + " aggregate qty " + std::to_string(lv.qty) +
                            " != sum of orders " + std::to_string(sum));
            seen += n;
        }
        if (seen != resting_) return fail("resting count mismatch");
        return true;
    }

#ifdef TACHYON_PLANTED_BUGS
    void set_bugs(std::uint32_t b) noexcept { bugs_ = b; }
#endif

private:
    struct Order {
        std::uint32_t next = kNil;
        std::uint32_t prev = kNil;
        OrderId id = 0;
        Price price = 0;
        Qty qty = 0;
        Side side = Side::Buy;
    };
    struct Level {
        std::uint32_t head = kNil;
        std::uint32_t tail = kNil;
        std::uint64_t qty = 0;
        std::uint32_t count = 0;
    };

    // Returns the unfilled remainder.
    template <class Sink>
    Qty match(OrderId taker, Side side, Price limit, Qty qty, Sink& sink) {
        if (side == Side::Buy) {
            const Price lim = TACHYON_BUG(kBugTradeThrough) ? limit + 1 : limit;
            while (qty != 0 && has_ask() && best_ask_ <= lim) qty = take_level(taker, Side::Sell, best_ask_, qty, sink);
        } else {
            const Price lim = TACHYON_BUG(kBugTradeThrough) ? limit - 1 : limit;
            while (qty != 0 && has_bid() && best_bid_ >= lim) qty = take_level(taker, Side::Buy, best_bid_, qty, sink);
        }
        return qty;
    }

    template <class Sink>
    Qty take_level(OrderId taker, Side maker_side, Price p, Qty qty, Sink& sink) {
        Level& lv = levels_[p];
        std::uint32_t s = lv.head;
        while (qty != 0 && s != kNil) {
            Order& m = pool_[s];
            const Qty t = std::min(qty, m.qty);
            sink.on_fill(Fill{taker, m.id, p, t});
            qty -= t;
            m.qty -= t;
            lv.qty -= t;
            if (TACHYON_BUG(kBugDustFill) && m.qty == 1) {
                m.qty = 0;
                lv.qty -= 1;
            }
            const std::uint32_t nx = m.next;
            if (m.qty == 0) unlink_and_free(s, lv);
            s = nx;
        }
        if (lv.count == 0) level_emptied(p, maker_side, /*from_cancel=*/false);
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        const std::uint32_t s = free_head_;
        free_head_ = pool_[s].next;
        Order& o = pool_[s];
        o.id = id;
        o.price = price;
        o.qty = qty;
        o.side = side;
        Level& lv = levels_[price];
        if (TACHYON_BUG(kBugLifoOnSecondOrder) && lv.count == 1) {
            o.prev = kNil;
            o.next = lv.head;
            pool_[lv.head].prev = s;
            lv.head = s;
        } else {
            o.next = kNil;
            o.prev = lv.tail;
            if (lv.tail != kNil) pool_[lv.tail].next = s;
            else lv.head = s;
            lv.tail = s;
        }
        ++lv.count;
        lv.qty += qty;
        index_[id] = s;
        ++resting_;
        if (side == Side::Buy) {
            set_bit(bid_bits_, price);
            if (price > best_bid_) best_bid_ = price;
        } else {
            set_bit(ask_bits_, price);
            if (price < best_ask_) best_ask_ = price;
        }
    }

    void remove(std::uint32_t s, bool from_cancel) {
        Order& o = pool_[s];
        Level& lv = levels_[o.price];
        const Price p = o.price;
        const Side side = o.side;
        if (!(from_cancel && TACHYON_BUG(kBugCancelLeaksLevelQty))) lv.qty -= o.qty;
        unlink_and_free(s, lv);
        if (lv.count == 0) level_emptied(p, side, from_cancel);
    }

    void unlink_and_free(std::uint32_t s, Level& lv) {
        Order& o = pool_[s];
        if (o.prev != kNil) pool_[o.prev].next = o.next;
        else lv.head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev;
        else lv.tail = o.prev;
        --lv.count;
        --resting_;
        index_[o.id] = kNil;
        o.next = free_head_;
        free_head_ = s;
    }

    void level_emptied(Price p, Side side, bool from_cancel) {
        const bool stale = from_cancel && TACHYON_BUG(kBugStaleBestOnCancel);
        if (side == Side::Buy) {
            clear_bit(bid_bits_, p);
            if (p == best_bid_ && !stale) best_bid_ = scan_down(bid_bits_, p - 1);
        } else {
            clear_bit(ask_bits_, p);
            if (p == best_ask_ && !stale) best_ask_ = scan_up(ask_bits_, p + 1);
        }
    }

    // ---- bitmap helpers ------------------------------------------------------------------

    static bool bit(const std::vector<std::uint64_t>& b, Price p) noexcept { return (b[p >> 6] >> (p & 63)) & 1u; }
    static void set_bit(std::vector<std::uint64_t>& b, Price p) noexcept { b[p >> 6] |= 1ull << (p & 63); }
    static void clear_bit(std::vector<std::uint64_t>& b, Price p) noexcept { b[p >> 6] &= ~(1ull << (p & 63)); }

    // Lowest set bit >= from, or num_ticks_ if none.
    Price scan_up(const std::vector<std::uint64_t>& b, Price from) const noexcept {
        if (TACHYON_BUG(kBugWordBoundaryScan) && from < num_ticks_ && (from & 63) == 63)
            return scan_up_raw(b, from + 1);  // skips bit 63 of the starting word
        return scan_up_raw(b, from);
    }
    // Highest set bit <= from, or -1 if none.
    Price scan_down(const std::vector<std::uint64_t>& b, Price from) const noexcept {
        if (TACHYON_BUG(kBugWordBoundaryScan) && from >= 0 && (from & 63) == 0)
            return scan_down_raw(b, from - 1);  // skips bit 0 of the starting word
        return scan_down_raw(b, from);
    }

    Price scan_up_raw(const std::vector<std::uint64_t>& b, Price from) const noexcept {
        if (from >= num_ticks_) return num_ticks_;
        std::size_t w = static_cast<std::size_t>(from) >> 6;
        std::uint64_t word = b[w] & (~0ull << (from & 63));
        for (;;) {
            if (word != 0) {
                const Price p = static_cast<Price>((w << 6) + std::countr_zero(word));
                return p < num_ticks_ ? p : num_ticks_;
            }
            if (++w == b.size()) return num_ticks_;
            word = b[w];
        }
    }
    Price scan_down_raw(const std::vector<std::uint64_t>& b, Price from) const noexcept {
        if (from < 0) return -1;
        std::size_t w = static_cast<std::size_t>(from) >> 6;
        std::uint64_t word = b[w] & (~0ull >> (63 - (from & 63)));
        for (;;) {
            if (word != 0) return static_cast<Price>((w << 6) + 63 - std::countl_zero(word));
            if (w-- == 0) return -1;
            word = b[w];
        }
    }

    Price num_ticks_;
    Price best_bid_;
    Price best_ask_;
    std::uint32_t free_head_ = kNil;
    std::uint32_t resting_ = 0;
    std::vector<Order> pool_;
    std::vector<std::uint32_t> index_;
    std::vector<Level> levels_;
    std::vector<std::uint64_t> bid_bits_;
    std::vector<std::uint64_t> ask_bits_;
#ifdef TACHYON_PLANTED_BUGS
    std::uint32_t bugs_ = 0;
#endif
};

}  // namespace tachyon
