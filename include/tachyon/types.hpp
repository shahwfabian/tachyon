#pragma once

#include <cstdint>

namespace tachyon {

using Price = std::int32_t;    // integer ticks, never floating point
using Qty = std::uint32_t;
using OrderId = std::uint32_t;  // exchange-assigned, dense

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

enum class Status : std::uint8_t { Ok = 0, Rejected = 1 };

// One execution. Always priced at the resting (maker) order's price.
struct Fill {
    OrderId taker;
    OrderId maker;
    Price price;
    Qty qty;
    friend bool operator==(const Fill&, const Fill&) = default;
};

// Sink that discards fills. Used by the benchmark hot path.
struct NullSink {
    void on_fill(const Fill&) noexcept {}
};

// Planted bugs. Compiled in only when TACHYON_PLANTED_BUGS is defined (the fuzz target),
// so the production and benchmark builds carry zero overhead. Each one is a realistic
// defect class seen in real matching engines.
enum Bug : std::uint32_t {
    kBugNone = 0,
    kBugDustFill = 1u << 0,             // maker left with 1 lot is silently treated as filled
    kBugTradeThrough = 1u << 1,         // aggressive limit order trades one tick through its limit
    kBugStaleBestOnCancel = 1u << 2,    // cancelling the last order at the best price keeps the stale best
    kBugCancelLeaksLevelQty = 1u << 3,  // cancel forgets to subtract from the level's aggregate quantity
    kBugModifyUpKeepsPriority = 1u << 4,// increasing size in place keeps time priority (must lose it)
    kBugLifoOnSecondOrder = 1u << 5,    // second order at a level is queued ahead of the first
    kBugWordBoundaryScan = 1u << 6,     // bitmap scan misses a level sitting on bit 63 of a word
    kBugMarketRemainderRests = 1u << 7, // unfilled market-order remainder rests on the book
};

constexpr std::uint32_t kAllBugs[] = {
    kBugDustFill,          kBugTradeThrough,         kBugStaleBestOnCancel, kBugCancelLeaksLevelQty,
    kBugModifyUpKeepsPriority, kBugLifoOnSecondOrder, kBugWordBoundaryScan,  kBugMarketRemainderRests,
};

}  // namespace tachyon
