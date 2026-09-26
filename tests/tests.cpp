// Unit tests. Deliberately dependency-free: a tiny CHECK macro, run as a plain executable.

#include <cstdio>
#include <string>
#include <vector>

#include "tachyon/order_book.hpp"

using namespace tachyon;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

struct Recorder {
    std::vector<Fill> fills;
    void on_fill(const Fill& f) { fills.push_back(f); }
};

static BookConfig small() {
    BookConfig c;
    c.num_ticks = 1024;
    c.max_order_id = 4096;
    c.max_resting = 4096;
    return c;
}

static void check_ok(const OrderBook& b) {
    std::string why;
    const bool ok = b.check_invariants(&why);
    CHECK(ok);
    if (!ok) std::printf("       invariant: %s\n", why.c_str());
}

static std::vector<OrderId> queue_at(const OrderBook& b, Price p) {
    std::vector<OrderId> ids;
    b.for_each_order_at(p, [&](OrderId id, Qty) { ids.push_back(id); });
    return ids;
}

static void test_empty_book() {
    OrderBook b(small());
    CHECK(!b.has_bid());
    CHECK(!b.has_ask());
    CHECK(b.resting_orders() == 0);
    check_ok(b);
}

static void test_rest_and_best_prices() {
    OrderBook b(small());
    Recorder r;
    CHECK(b.add(1, Side::Buy, 100, 5, r) == Status::Ok);
    CHECK(b.add(2, Side::Buy, 101, 5, r) == Status::Ok);
    CHECK(b.add(3, Side::Sell, 105, 5, r) == Status::Ok);
    CHECK(b.add(4, Side::Sell, 103, 5, r) == Status::Ok);
    CHECK(r.fills.empty());
    CHECK(b.best_bid() == 101);
    CHECK(b.best_ask() == 103);
    CHECK(b.resting_orders() == 4);
    check_ok(b);
}

static void test_full_fill_at_maker_price() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 10, r);
    b.add(2, Side::Buy, 102, 10, r);  // crosses; must trade at the maker's 100, not 102
    CHECK(r.fills.size() == 1);
    CHECK((r.fills[0] == Fill{2, 1, 100, 10}));
    CHECK(!b.has_bid());
    CHECK(!b.has_ask());
    check_ok(b);
}

static void test_partial_fill_and_remainder_rests() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 4, r);
    b.add(2, Side::Buy, 100, 10, r);
    CHECK(r.fills.size() == 1 && r.fills[0].qty == 4);
    CHECK(b.best_bid() == 100);
    CHECK(b.level_qty(100) == 6);
    CHECK(!b.has_ask());
    check_ok(b);
}

static void test_fifo_priority_within_level() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 3, r);
    b.add(2, Side::Sell, 100, 3, r);
    b.add(3, Side::Sell, 100, 3, r);
    b.market(9, Side::Buy, 5, r);
    CHECK(r.fills.size() == 2);
    CHECK((r.fills[0] == Fill{9, 1, 100, 3}));
    CHECK((r.fills[1] == Fill{9, 2, 100, 2}));
    CHECK((queue_at(b, 100) == std::vector<OrderId>{2, 3}));
    check_ok(b);
}

static void test_sweep_multiple_levels_respects_limit() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 2, r);
    b.add(2, Side::Sell, 101, 2, r);
    b.add(3, Side::Sell, 102, 2, r);
    b.add(4, Side::Buy, 101, 10, r);  // takes 100 and 101, must not touch 102
    CHECK(r.fills.size() == 2);
    CHECK(r.fills[1].price == 101);
    CHECK(b.best_ask() == 102);
    CHECK(b.best_bid() == 101);
    CHECK(b.level_qty(101) == 6);
    check_ok(b);
}

static void test_market_order_never_rests() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Buy, 50, 3, r);
    CHECK(b.market(2, Side::Sell, 10, r) == Status::Ok);
    CHECK(r.fills.size() == 1 && r.fills[0].qty == 3);
    CHECK(!b.has_bid());
    CHECK(!b.has_ask());
    CHECK(b.resting_orders() == 0);
    check_ok(b);
}

static void test_cancel() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Buy, 100, 5, r);
    b.add(2, Side::Buy, 99, 5, r);
    CHECK(b.cancel(1) == Status::Ok);
    CHECK(b.best_bid() == 99);
    CHECK(b.cancel(1) == Status::Rejected);  // already gone
    CHECK(b.cancel(777) == Status::Rejected);  // never existed
    check_ok(b);
}

static void test_modify_down_keeps_priority() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 5, r);
    b.add(2, Side::Sell, 100, 5, r);
    CHECK(b.modify(1, 100, 2, r) == Status::Ok);
    CHECK((queue_at(b, 100) == std::vector<OrderId>{1, 2}));
    CHECK(b.level_qty(100) == 7);
    check_ok(b);
}

static void test_modify_up_loses_priority() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 5, r);
    b.add(2, Side::Sell, 100, 5, r);
    CHECK(b.modify(1, 100, 9, r) == Status::Ok);
    CHECK((queue_at(b, 100) == std::vector<OrderId>{2, 1}));
    CHECK(b.level_qty(100) == 14);
    check_ok(b);
}

static void test_modify_reprice_can_trade() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 105, 5, r);
    b.add(2, Side::Buy, 100, 5, r);
    CHECK(b.modify(2, 105, 5, r) == Status::Ok);
    CHECK(r.fills.size() == 1 && r.fills[0].price == 105);
    CHECK(b.resting_orders() == 0);
    check_ok(b);
}

static void test_rejects() {
    OrderBook b(small());
    Recorder r;
    CHECK(b.add(1, Side::Buy, 100, 0, r) == Status::Rejected);     // zero qty
    CHECK(b.add(1, Side::Buy, -1, 5, r) == Status::Rejected);      // below grid
    CHECK(b.add(1, Side::Buy, 1024, 5, r) == Status::Rejected);    // above grid
    CHECK(b.add(5000, Side::Buy, 10, 5, r) == Status::Rejected);   // id out of range
    CHECK(b.add(1, Side::Buy, 100, 5, r) == Status::Ok);
    CHECK(b.add(1, Side::Buy, 100, 5, r) == Status::Rejected);     // duplicate live id
    CHECK(b.modify(2, 100, 5, r) == Status::Rejected);             // unknown id
    check_ok(b);
}

static void test_best_price_scan_across_word_boundaries() {
    OrderBook b(small());
    Recorder r;
    // Asks straddling the 64-bit word boundaries at 63/64 and 127/128.
    b.add(1, Side::Sell, 62, 1, r);
    b.add(2, Side::Sell, 63, 1, r);
    b.add(3, Side::Sell, 64, 1, r);
    b.add(4, Side::Sell, 128, 1, r);
    b.market(10, Side::Buy, 1, r);
    CHECK(b.best_ask() == 63);
    b.market(11, Side::Buy, 1, r);
    CHECK(b.best_ask() == 64);
    b.market(12, Side::Buy, 1, r);
    CHECK(b.best_ask() == 128);
    // Bids straddling the same boundaries, walked downward.
    b.add(20, Side::Buy, 65, 1, r);
    b.add(21, Side::Buy, 64, 1, r);
    b.add(22, Side::Buy, 63, 1, r);
    b.add(23, Side::Buy, 0, 1, r);
    b.market(30, Side::Sell, 1, r);
    CHECK(b.best_bid() == 64);
    b.market(31, Side::Sell, 1, r);
    CHECK(b.best_bid() == 63);
    b.market(32, Side::Sell, 1, r);
    CHECK(b.best_bid() == 0);
    check_ok(b);
}

static void test_level_flips_side() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 5, r);
    b.add(2, Side::Buy, 100, 8, r);  // consumes the ask, rests 3 as a bid at the same tick
    CHECK(!b.has_ask());
    CHECK(b.best_bid() == 100);
    CHECK(b.level_qty(100) == 3);
    check_ok(b);
}

static void test_id_reuse_after_fill() {
    OrderBook b(small());
    Recorder r;
    b.add(1, Side::Sell, 100, 5, r);
    b.market(2, Side::Buy, 5, r);
    CHECK(b.add(1, Side::Sell, 100, 5, r) == Status::Ok);  // id 1 is no longer live
    check_ok(b);
}

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"empty book", test_empty_book},
        {"rest and best prices", test_rest_and_best_prices},
        {"full fill at maker price", test_full_fill_at_maker_price},
        {"partial fill, remainder rests", test_partial_fill_and_remainder_rests},
        {"FIFO priority within a level", test_fifo_priority_within_level},
        {"sweep respects limit price", test_sweep_multiple_levels_respects_limit},
        {"market order never rests", test_market_order_never_rests},
        {"cancel", test_cancel},
        {"modify down keeps priority", test_modify_down_keeps_priority},
        {"modify up loses priority", test_modify_up_loses_priority},
        {"modify reprice can trade", test_modify_reprice_can_trade},
        {"rejects", test_rejects},
        {"best-price scan across word boundaries", test_best_price_scan_across_word_boundaries},
        {"level flips side", test_level_flips_side},
        {"id reuse after fill", test_id_reuse_after_fill},
    };
    for (const auto& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "[ ok ]" : "[FAIL]", t.name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
