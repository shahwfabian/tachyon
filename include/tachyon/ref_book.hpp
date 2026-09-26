#pragma once

// Reference order book: the "obviously correct" version.
//
// Written for clarity, not speed: std::map of price -> std::list FIFO queue, plus a hash map
// from id to location. The fuzzer runs it in lockstep with OrderBook and demands identical
// fills and identical book state after every event (differential testing). The benchmark
// also uses it as the baseline Tachyon is measured against.

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "order_book.hpp"
#include "types.hpp"

namespace tachyon {

class RefBook {
public:
    explicit RefBook(const BookConfig& cfg = {}) : num_ticks_(cfg.num_ticks), max_id_(cfg.max_order_id) {}

    template <class Sink>
    Status add(OrderId id, Side side, Price price, Qty qty, Sink&& sink) {
        if (qty == 0 || price < 0 || price >= num_ticks_ || id >= max_id_ || live_.count(id)) return Status::Rejected;
        qty = side == Side::Buy ? match(asks_, id, [&](Price p) { return p <= price; }, qty, sink)
                                : match(bids_, id, [&](Price p) { return p >= price; }, qty, sink);
        if (qty != 0) {
            auto& queue = side == Side::Buy ? bids_[price] : asks_[price];
            queue.push_back({id, qty});
            live_[id] = {side, price, std::prev(queue.end())};
        }
        return Status::Ok;
    }

    template <class Sink>
    Status market(OrderId id, Side side, Qty qty, Sink&& sink) {
        if (qty == 0 || id >= max_id_ || live_.count(id)) return Status::Rejected;
        if (side == Side::Buy) match(asks_, id, [](Price) { return true; }, qty, sink);
        else match(bids_, id, [](Price) { return true; }, qty, sink);
        return Status::Ok;
    }

    Status cancel(OrderId id) {
        auto it = live_.find(id);
        if (it == live_.end()) return Status::Rejected;
        erase(it);
        return Status::Ok;
    }

    template <class Sink>
    Status modify(OrderId id, Price price, Qty qty, Sink&& sink) {
        auto it = live_.find(id);
        if (it == live_.end() || price < 0 || price >= num_ticks_) return Status::Rejected;
        if (qty == 0) return cancel(id);
        Loc& loc = it->second;
        if (loc.price == price && qty <= loc.pos->qty) {
            loc.pos->qty = qty;  // keeps its place in the queue
            return Status::Ok;
        }
        const Side side = loc.side;
        erase(it);
        return add(id, side, price, qty, sink);
    }

    struct Resting {
        OrderId id;
        Qty qty;
    };
    using Queue = std::list<Resting>;

    // Best-to-worst iteration helpers for comparisons.
    const std::map<Price, Queue, std::greater<Price>>& bids() const { return bids_; }
    const std::map<Price, Queue>& asks() const { return asks_; }
    std::size_t resting_orders() const { return live_.size(); }

private:
    struct Loc {
        Side side;
        Price price;
        Queue::iterator pos;
    };

    template <class Map, class Crosses, class Sink>
    Qty match(Map& book, OrderId taker, Crosses crosses, Qty qty, Sink& sink) {
        while (qty != 0 && !book.empty() && crosses(book.begin()->first)) {
            auto lvl = book.begin();
            Queue& q = lvl->second;
            while (qty != 0 && !q.empty()) {
                Resting& m = q.front();
                const Qty t = std::min(qty, m.qty);
                sink.on_fill(Fill{taker, m.id, lvl->first, t});
                qty -= t;
                m.qty -= t;
                if (m.qty == 0) {
                    live_.erase(m.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(lvl);
        }
        return qty;
    }

    void erase(std::unordered_map<OrderId, Loc>::iterator it) {
        const Loc loc = it->second;
        if (loc.side == Side::Buy) {
            auto lvl = bids_.find(loc.price);
            lvl->second.erase(loc.pos);
            if (lvl->second.empty()) bids_.erase(lvl);
        } else {
            auto lvl = asks_.find(loc.price);
            lvl->second.erase(loc.pos);
            if (lvl->second.empty()) asks_.erase(lvl);
        }
        live_.erase(it);
    }

    Price num_ticks_;
    OrderId max_id_;
    std::map<Price, Queue, std::greater<Price>> bids_;
    std::map<Price, Queue> asks_;
    std::unordered_map<OrderId, Loc> live_;
};

}  // namespace tachyon
