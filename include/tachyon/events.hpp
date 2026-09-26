#pragma once

// Order-entry events: the common input format for tests, the fuzzer and the benchmark.

#include <cstdint>
#include <string>

#include "types.hpp"

namespace tachyon {

enum class EventType : std::uint8_t { Add = 0, Market = 1, Cancel = 2, Modify = 3 };

struct Event {
    EventType type;
    Side side;  // Add / Market
    OrderId id;
    Price price;  // Add / Modify
    Qty qty;      // Add / Market / Modify
};

template <class Book, class Sink>
inline Status apply(Book& book, const Event& e, Sink&& sink) {
    switch (e.type) {
        case EventType::Add: return book.add(e.id, e.side, e.price, e.qty, sink);
        case EventType::Market: return book.market(e.id, e.side, e.qty, sink);
        case EventType::Cancel: return book.cancel(e.id);
        case EventType::Modify: return book.modify(e.id, e.price, e.qty, sink);
    }
    return Status::Rejected;
}

inline std::string to_string(const Event& e) {
    const char* side = e.side == Side::Buy ? "BUY" : "SELL";
    switch (e.type) {
        case EventType::Add:
            return "ADD #" + std::to_string(e.id) + " " + side + " " + std::to_string(e.qty) + " @ " + std::to_string(e.price);
        case EventType::Market:
            return "MKT #" + std::to_string(e.id) + " " + side + " " + std::to_string(e.qty);
        case EventType::Cancel: return "CXL #" + std::to_string(e.id);
        case EventType::Modify:
            return "MOD #" + std::to_string(e.id) + " -> " + std::to_string(e.qty) + " @ " + std::to_string(e.price);
    }
    return "?";
}

}  // namespace tachyon
