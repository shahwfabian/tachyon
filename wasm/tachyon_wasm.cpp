// C ABI over the real OrderBook, compiled to WebAssembly for the live demo on the website.
// The browser drives the same engine code that is benchmarked and fuzzed.

#include <cstdint>

#include "tachyon/order_book.hpp"

using namespace tachyon;

namespace {

struct FillBuffer {
    static constexpr int kCap = 4096;
    std::int32_t data[kCap * 4];
    int n = 0;
    void on_fill(const Fill& f) {
        if (n == kCap) return;
        data[n * 4 + 0] = static_cast<std::int32_t>(f.taker);
        data[n * 4 + 1] = static_cast<std::int32_t>(f.maker);
        data[n * 4 + 2] = f.price;
        data[n * 4 + 3] = static_cast<std::int32_t>(f.qty);
        ++n;
    }
};

OrderBook* g_book = nullptr;
FillBuffer g_fills;
std::int32_t g_depth[3 * 512];

}  // namespace

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT void tk_init(std::int32_t ticks, std::int32_t max_ids) {
    delete g_book;
    BookConfig c;
    c.num_ticks = ticks;
    c.max_order_id = static_cast<std::uint32_t>(max_ids);
    c.max_resting = 1u << 16;
    g_book = new OrderBook(c);
}

EXPORT std::int32_t tk_add(std::uint32_t id, std::int32_t side, std::int32_t price, std::uint32_t qty) {
    g_fills.n = 0;
    return g_book->add(id, side ? Side::Sell : Side::Buy, price, qty, g_fills) == Status::Ok ? g_fills.n : -1;
}

EXPORT std::int32_t tk_market(std::uint32_t id, std::int32_t side, std::uint32_t qty) {
    g_fills.n = 0;
    return g_book->market(id, side ? Side::Sell : Side::Buy, qty, g_fills) == Status::Ok ? g_fills.n : -1;
}

EXPORT std::int32_t tk_cancel(std::uint32_t id) { return g_book->cancel(id) == Status::Ok ? 0 : -1; }

EXPORT std::int32_t tk_modify(std::uint32_t id, std::int32_t price, std::uint32_t qty) {
    g_fills.n = 0;
    return g_book->modify(id, price, qty, g_fills) == Status::Ok ? g_fills.n : -1;
}

EXPORT std::int32_t* tk_fills() { return g_fills.data; }
EXPORT std::int32_t tk_best_bid() { return g_book->best_bid(); }
EXPORT std::int32_t tk_best_ask() { return g_book->best_ask(); }
EXPORT std::int32_t tk_resting() { return static_cast<std::int32_t>(g_book->resting_orders()); }

// Writes up to `max` levels as (price, qty, count) triples, best first. Returns the count.
EXPORT std::int32_t tk_depth(std::int32_t side, std::int32_t max) {
    int n = 0;
    if (max > 512) max = 512;
    g_book->for_each_level(side ? Side::Sell : Side::Buy, [&](Price p, std::uint64_t q, std::uint32_t c) {
        if (n >= max) return;
        g_depth[n * 3 + 0] = p;
        g_depth[n * 3 + 1] = static_cast<std::int32_t>(q);
        g_depth[n * 3 + 2] = static_cast<std::int32_t>(c);
        ++n;
    });
    return n;
}
EXPORT std::int32_t* tk_depth_ptr() { return g_depth; }
