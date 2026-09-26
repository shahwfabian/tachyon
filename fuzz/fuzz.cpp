// Differential fuzzer with test-case shrinking.
//
// For each planted bug:
//   1. Generate seeded random order-entry sequences (adds, market orders, cancels, modifies).
//   2. Replay each one through Tachyon (bug enabled) and the reference book in lockstep.
//      After every event, demand identical status, identical fills, identical book state,
//      plus Tachyon's own structural invariants and oracle-free trading rules.
//   3. On the first failure, shrink: delete chunks of events and simplify values while the
//      same kind of failure still reproduces, until nothing more can be removed.
//   4. Record a step-by-step trace of the minimal reproduction for the web viewer.
//
// A clean campaign (no bugs enabled) runs first to show the harness has no false positives.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "tachyon/events.hpp"
#include "tachyon/order_book.hpp"
#include "tachyon/ref_book.hpp"

using namespace tachyon;
using Clock = std::chrono::steady_clock;

// ---- configuration --------------------------------------------------------------------------

constexpr Price kTicks = 1024;
constexpr std::uint32_t kSeqLen = 400;
static std::uint32_t kCleanSequences = 25000;  // --clean N overrides
constexpr std::uint32_t kMaxSequencesPerBug = 200000;

static BookConfig fuzz_config() {
    BookConfig c;
    c.num_ticks = kTicks;
    c.max_order_id = 4 * kSeqLen + 16;
    c.max_resting = 4 * kSeqLen + 16;
    return c;
}

// ---- random generation ----------------------------------------------------------------------

struct Rng {  // splitmix64: tiny, fast, good enough for test generation
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
    bool chance(std::uint32_t pct) { return below(100) < pct; }
};

// Realistic-ish flow: a mid price that random-walks, passive quotes clustered near it,
// occasional aggressive orders, and cancels/modifies aimed at previously sent orders.
static std::vector<Event> generate(std::uint64_t seed, std::uint32_t len) {
    Rng r(seed);
    std::vector<Event> ev;
    ev.reserve(len);
    Price mid = 64 + static_cast<Price>(r.below(kTicks - 128));
    OrderId next_id = 1;
    std::vector<OrderId> sent;
    auto small_qty = [&] { return r.chance(60) ? 1 + r.below(3) : 1 + r.below(12); };
    for (std::uint32_t i = 0; i < len; ++i) {
        if (r.chance(10)) mid += r.chance(50) ? 1 : -1;
        mid = std::clamp<Price>(mid, 16, kTicks - 17);
        const std::uint32_t roll = r.below(100);
        const Side side = r.chance(50) ? Side::Buy : Side::Sell;
        if (roll < 50 || sent.empty()) {
            const bool aggressive = r.chance(20);
            const Price off = static_cast<Price>(r.below(6)) + (aggressive ? 0 : 1);
            const Price px = (side == Side::Buy) == aggressive ? mid + off : mid - off;
            ev.push_back({EventType::Add, side, next_id, px, small_qty()});
            sent.push_back(next_id++);
        } else if (roll < 62) {
            ev.push_back({EventType::Market, side, next_id++, 0, small_qty()});
        } else if (roll < 82) {
            ev.push_back({EventType::Cancel, side, sent[r.below(static_cast<std::uint32_t>(sent.size()))], 0, 0});
        } else {
            const OrderId id = sent[r.below(static_cast<std::uint32_t>(sent.size()))];
            const Price px = mid + static_cast<Price>(r.below(9)) - 4;
            ev.push_back({EventType::Modify, side, id, px, small_qty()});
        }
    }
    return ev;
}

// ---- lockstep execution ---------------------------------------------------------------------

struct Recorder {
    std::vector<Fill> fills;
    void on_fill(const Fill& f) { fills.push_back(f); }
};

struct LevelSnap {
    Price price;
    std::uint64_t qty;
    std::vector<std::pair<OrderId, Qty>> orders;
    bool operator==(const LevelSnap&) const = default;
};
struct BookSnap {
    std::vector<LevelSnap> bids, asks;
    bool operator==(const BookSnap&) const = default;
};

static BookSnap snap(const OrderBook& b) {
    BookSnap s;
    for (Side side : {Side::Buy, Side::Sell}) {
        auto& out = side == Side::Buy ? s.bids : s.asks;
        b.for_each_level(side, [&](Price p, std::uint64_t q, std::uint32_t) {
            LevelSnap l{p, q, {}};
            b.for_each_order_at(p, [&](OrderId id, Qty oq) { l.orders.push_back({id, oq}); });
            out.push_back(std::move(l));
        });
    }
    return s;
}

static BookSnap snap(const RefBook& b) {
    BookSnap s;
    auto fill = [](auto& map, std::vector<LevelSnap>& out) {
        for (auto& [p, q] : map) {
            LevelSnap l{p, 0, {}};
            for (auto& o : q) {
                l.orders.push_back({o.id, o.qty});
                l.qty += o.qty;
            }
            out.push_back(std::move(l));
        }
    };
    fill(b.bids(), s.bids);
    fill(b.asks(), s.asks);
    return s;
}

struct Failure {
    std::size_t step;  // index of the event that exposed it
    std::string kind;  // category, used to keep shrinking on the same bug
    std::string message;
};

// Per-step detail, only collected when recording the final trace.
struct StepRecord {
    Event event;
    Status status, ref_status;
    std::vector<Fill> fills, ref_fills;
    BookSnap book, ref_book;
    Price best_bid, best_ask, ref_best_bid, ref_best_ask;  // cached vs. true top of book
};

static std::optional<Failure> run(const std::vector<Event>& ev, std::uint32_t bugs,
                                  std::vector<StepRecord>* trace = nullptr) {
    OrderBook book(fuzz_config());
#ifdef TACHYON_PLANTED_BUGS
    book.set_bugs(bugs);
#else
    (void)bugs;
#endif
    RefBook ref(fuzz_config());
    Recorder a, b;
    for (std::size_t i = 0; i < ev.size(); ++i) {
        const Event& e = ev[i];
        a.fills.clear();
        b.fills.clear();
        const Status sa = apply(book, e, a);
        const Status sb = apply(ref, e, b);
        std::optional<Failure> f;

        std::string why;
        if (!book.check_invariants(&why)) f = Failure{i, "invariant", why};

        // Oracle-free trading rules: no trade-through, fills never exceed the order.
        if (!f && e.type == EventType::Add) {
            Qty total = 0;
            for (const Fill& x : a.fills) {
                total += x.qty;
                if ((e.side == Side::Buy && x.price > e.price) || (e.side == Side::Sell && x.price < e.price))
                    f = Failure{i, "trade-through",
                                "order #" + std::to_string(e.id) + " limit " + std::to_string(e.price) +
                                    " traded at " + std::to_string(x.price)};
            }
            if (!f && total > e.qty) f = Failure{i, "overfill", "fills exceed order quantity"};
        }
        if (!f && e.type == EventType::Market) {
            bool rests = false;
            for (Side s : {Side::Buy, Side::Sell})
                book.for_each_level(s, [&](Price p, std::uint64_t, std::uint32_t) {
                    book.for_each_order_at(p, [&](OrderId id, Qty) { rests |= id == e.id; });
                });
            if (rests) f = Failure{i, "market-rests", "market order #" + std::to_string(e.id) + " is resting on the book"};
        }

        // Differential checks against the reference.
        if (!f && sa != sb)
            f = Failure{i, "status", std::string("tachyon ") + (sa == Status::Ok ? "accepted" : "rejected") +
                                         ", reference " + (sb == Status::Ok ? "accepted" : "rejected")};
        if (!f && a.fills != b.fills) {
            std::ostringstream m;
            m << "fills diverge: tachyon " << a.fills.size() << " fill(s), reference " << b.fills.size();
            for (std::size_t k = 0; k < std::max(a.fills.size(), b.fills.size()); ++k) {
                if (k < a.fills.size() && k < b.fills.size() && a.fills[k] == b.fills[k]) continue;
                m << "; first difference at fill " << k;
                break;
            }
            f = Failure{i, "fills", m.str()};
        }
        BookSnap sa_book, sb_book;
        if (!f || trace) {
            sa_book = snap(book);
            sb_book = snap(ref);
        }
        if (!f && sa_book != sb_book) f = Failure{i, "book", "book state diverges from reference"};

        if (trace) {
            const Price rbb = ref.bids().empty() ? -1 : ref.bids().begin()->first;
            const Price rba = ref.asks().empty() ? kTicks : ref.asks().begin()->first;
            trace->push_back({e, sa, sb, a.fills, b.fills, std::move(sa_book), std::move(sb_book), book.best_bid(),
                              book.best_ask(), rbb, rba});
        }
        if (f) return f;
    }
    return std::nullopt;
}

// ---- shrinking ------------------------------------------------------------------------------

struct ShrinkStats {
    std::uint32_t runs = 0;
};

static std::vector<Event> shrink(std::vector<Event> ev, std::uint32_t bugs, const std::string& kind, ShrinkStats& st) {
    auto failing = [&](const std::vector<Event>& cand) -> std::optional<Failure> {
        ++st.runs;
        auto f = run(cand, bugs);
        if (f && f->kind == kind) return f;
        return std::nullopt;
    };
    auto truncate = [&](std::vector<Event>& v) {
        if (auto f = failing(v)) v.resize(f->step + 1);
    };
    truncate(ev);

    bool progress = true;
    while (progress) {
        progress = false;
        // Pass 1: delete chunks, halving the chunk size (delta debugging).
        for (std::size_t chunk = std::max<std::size_t>(ev.size() / 2, 1); chunk >= 1; chunk /= 2) {
            for (std::size_t i = 0; i + chunk <= ev.size();) {
                std::vector<Event> cand;
                cand.reserve(ev.size() - chunk);
                cand.insert(cand.end(), ev.begin(), ev.begin() + static_cast<std::ptrdiff_t>(i));
                cand.insert(cand.end(), ev.begin() + static_cast<std::ptrdiff_t>(i + chunk), ev.end());
                if (auto f = failing(cand)) {
                    cand.resize(f->step + 1);
                    ev = std::move(cand);
                    progress = true;
                } else {
                    i += chunk;
                }
            }
            if (chunk == 1) break;
        }
        // Pass 2: simplify values. Smaller quantities, then prices pulled toward a common anchor.
        Price anchor = -1;
        for (const Event& e : ev)
            if (e.type == EventType::Add) {
                anchor = e.price;
                break;
            }
        for (std::size_t i = 0; i < ev.size(); ++i) {
            for (Qty q : {Qty{1}, Qty{2}, ev[i].qty / 2, ev[i].qty - 1}) {
                if (q == 0 || q >= ev[i].qty || ev[i].type == EventType::Cancel) continue;
                auto cand = ev;
                cand[i].qty = q;
                if (failing(cand)) {
                    ev = std::move(cand);
                    progress = true;
                    break;
                }
            }
            if (anchor >= 0 && (ev[i].type == EventType::Add || ev[i].type == EventType::Modify) &&
                ev[i].price != anchor) {
                auto cand = ev;
                cand[i].price += cand[i].price < anchor ? 1 : -1;
                if (failing(cand)) {
                    ev = std::move(cand);
                    progress = true;
                }
            }
        }
    }

    // Cosmetic: renumber ids 1..k in order of first appearance so the repro reads cleanly.
    std::unordered_map<OrderId, OrderId> remap;
    auto rn = [&](OrderId id) {
        auto [it, inserted] = remap.try_emplace(id, static_cast<OrderId>(remap.size() + 1));
        return it->second;
    };
    auto renamed = ev;
    for (Event& e : renamed) e.id = rn(e.id);
    if (failing(renamed)) ev = std::move(renamed);
    return ev;
}

// ---- JSON output ----------------------------------------------------------------------------

static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

static std::string json_fills(const std::vector<Fill>& fs) {
    std::string o = "[";
    for (std::size_t i = 0; i < fs.size(); ++i) {
        if (i) o += ",";
        o += "[" + std::to_string(fs[i].taker) + "," + std::to_string(fs[i].maker) + "," + std::to_string(fs[i].price) +
             "," + std::to_string(fs[i].qty) + "]";
    }
    return o + "]";
}

static std::string json_levels(const std::vector<LevelSnap>& ls) {
    std::string o = "[";
    for (std::size_t i = 0; i < ls.size(); ++i) {
        if (i) o += ",";
        o += "{\"p\":" + std::to_string(ls[i].price) + ",\"q\":" + std::to_string(ls[i].qty) + ",\"o\":[";
        for (std::size_t k = 0; k < ls[i].orders.size(); ++k) {
            if (k) o += ",";
            o += "[" + std::to_string(ls[i].orders[k].first) + "," + std::to_string(ls[i].orders[k].second) + "]";
        }
        o += "]}";
    }
    return o + "]";
}

static std::string json_event(const Event& e) {
    static const char* types[] = {"add", "market", "cancel", "modify"};
    return "{\"type\":\"" + std::string(types[static_cast<int>(e.type)]) + "\",\"side\":\"" +
           (e.side == Side::Buy ? "buy" : "sell") + "\",\"id\":" + std::to_string(e.id) +
           ",\"price\":" + std::to_string(e.price) + ",\"qty\":" + std::to_string(e.qty) + ",\"text\":\"" +
           esc(to_string(e)) + "\"}";
}

struct BugInfo {
    std::uint32_t flag;
    const char* key;
    const char* name;
    const char* description;
};

static const BugInfo kBugInfo[] = {
    {kBugDustFill, "dust_fill", "Dust fill",
     "A maker order left with exactly 1 lot after a partial fill is silently marked filled."},
    {kBugTradeThrough, "trade_through", "Trade-through",
     "An aggressive limit order is allowed to trade one tick worse than its limit price."},
    {kBugStaleBestOnCancel, "stale_best", "Stale best price on cancel",
     "Cancelling the last order at the best level leaves the cached best price pointing at an empty level."},
    {kBugCancelLeaksLevelQty, "cancel_qty_leak", "Level quantity leak on cancel",
     "Cancel removes the order but forgets to subtract it from the level's aggregate quantity."},
    {kBugModifyUpKeepsPriority, "modify_up_priority", "Size-up keeps priority",
     "Increasing an order's size in place keeps its queue position instead of sending it to the back."},
    {kBugLifoOnSecondOrder, "lifo_second", "LIFO on second order",
     "The second order to join a price level is queued ahead of the first, breaking time priority."},
    {kBugWordBoundaryScan, "word_boundary", "Bitmap word-boundary scan",
     "The next-best-price scan skips a level sitting exactly on a 64-bit word boundary."},
    {kBugMarketRemainderRests, "market_rests", "Market remainder rests",
     "The unfilled remainder of a market order is left resting on the book at the last traded price."},
};

static double secs_since(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

int main(int argc, char** argv) {
    std::string out_path = "results/fuzz.json";
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--out")) out_path = argv[i + 1];
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--clean")) kCleanSequences = static_cast<std::uint32_t>(std::stoul(argv[i + 1]));

    std::ostringstream js;
    js << "{\n\"config\":{\"ticks\":" << kTicks << ",\"sequence_length\":" << kSeqLen << "},\n";

    // Clean campaign: prove zero false positives.
    {
        const auto t0 = Clock::now();
        std::uint64_t events = 0, failures = 0;
        for (std::uint64_t seed = 1; seed <= kCleanSequences; ++seed) {
            const auto ev = generate(seed, kSeqLen);
            events += ev.size();
            if (auto f = run(ev, kBugNone)) {
                ++failures;
                std::printf("CLEAN RUN FAILURE seed=%llu step=%zu: %s\n", static_cast<unsigned long long>(seed),
                            f->step, f->message.c_str());
            }
        }
        const double s = secs_since(t0);
        std::printf("clean engine: %u sequences, %llu events, %llu failures (%.1fs)\n\n", kCleanSequences,
                    static_cast<unsigned long long>(events), static_cast<unsigned long long>(failures), s);
        js << "\"clean\":{\"sequences\":" << kCleanSequences << ",\"events\":" << events << ",\"failures\":" << failures
           << ",\"seconds\":" << s << "},\n";
    }

    js << "\"bugs\":[\n";
    std::uint32_t found = 0;
    std::printf("%-30s %8s %10s %10s %8s %8s\n", "planted bug", "found", "sequences", "failed@", "shrunk", "runs");
    for (std::size_t bi = 0; bi < std::size(kBugInfo); ++bi) {
        const BugInfo& bug = kBugInfo[bi];
        const auto t0 = Clock::now();
        std::optional<Failure> fail;
        std::vector<Event> ev;
        std::uint64_t seed = 0, events = 0;
        while (!fail && seed < kMaxSequencesPerBug) {
            ev = generate(1000000 + seed++, kSeqLen);
            fail = run(ev, bug.flag);
            events += fail ? fail->step + 1 : ev.size();
        }
        const double find_s = secs_since(t0);
        if (bi) js << ",\n";
        js << "{\"key\":\"" << bug.key << "\",\"name\":\"" << esc(bug.name) << "\",\"description\":\""
           << esc(bug.description) << "\",\"found\":" << (fail ? "true" : "false") << ",\"sequences_tried\":" << seed
           << ",\"events_executed\":" << events << ",\"seconds_to_find\":" << find_s;
        if (!fail) {
            js << "}";
            std::printf("%-30s %8s %10llu\n", bug.name, "no", static_cast<unsigned long long>(seed));
            continue;
        }
        ++found;
        const std::size_t original = fail->step + 1;
        ShrinkStats st;
        const auto t1 = Clock::now();
        const auto small = shrink(ev, bug.flag, fail->kind, st);
        const double shrink_s = secs_since(t1);
        std::vector<StepRecord> trace;
        const auto final_fail = run(small, bug.flag, &trace);
        std::printf("%-30s %8s %10llu %10zu %8zu %8u\n", bug.name, "yes", static_cast<unsigned long long>(seed),
                    original, small.size(), st.runs);
        js << ",\"failure_kind\":\"" << esc(final_fail->kind) << "\",\"failure\":\"" << esc(final_fail->message)
           << "\",\"original_length\":" << original << ",\"shrunk_length\":" << small.size()
           << ",\"shrink_runs\":" << st.runs << ",\"shrink_seconds\":" << shrink_s << ",\"trace\":[";
        for (std::size_t k = 0; k < trace.size(); ++k) {
            const StepRecord& r = trace[k];
            if (k) js << ",";
            js << "\n  {\"event\":" << json_event(r.event) << ",\"status\":\"" << (r.status == Status::Ok ? "ok" : "rejected")
               << "\",\"ref_status\":\"" << (r.ref_status == Status::Ok ? "ok" : "rejected")
               << "\",\"fills\":" << json_fills(r.fills) << ",\"ref_fills\":" << json_fills(r.ref_fills)
               << ",\"bids\":" << json_levels(r.book.bids) << ",\"asks\":" << json_levels(r.book.asks)
               << ",\"ref_bids\":" << json_levels(r.ref_book.bids) << ",\"ref_asks\":" << json_levels(r.ref_book.asks)
               << ",\"bbo\":[" << r.best_bid << "," << r.best_ask << "],\"ref_bbo\":[" << r.ref_best_bid << ","
               << r.ref_best_ask << "]}";
        }
        js << "]}";
    }
    js << "\n],\n\"summary\":{\"found\":" << found << ",\"total\":" << std::size(kBugInfo) << "}\n}\n";
    std::printf("\nfound %u / %zu planted bugs\n", found, std::size(kBugInfo));

    std::ofstream(out_path) << js.str();
    std::printf("wrote %s\n", out_path.c_str());
    return found == std::size(kBugInfo) ? 0 : 1;
}
