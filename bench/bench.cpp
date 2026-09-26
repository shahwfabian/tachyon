// Latency and throughput benchmark: Tachyon vs. the std::map reference book.
//
// Both books replay the identical pre-generated event stream (so generation cost is excluded),
// starting from the identical pre-warmed book. Latency is measured per operation with the TSC;
// throughput is measured separately without per-op timing, median of several runs.
// Both books must produce the same fill count and volume, which is checked at the end.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#include <x86intrin.h>
#define TACHYON_HAVE_TSC 1
#endif

#include "tachyon/events.hpp"
#include "tachyon/order_book.hpp"
#include "tachyon/ref_book.hpp"

using namespace tachyon;
using Clock = std::chrono::steady_clock;

constexpr Price kTicks = 1 << 16;
constexpr std::uint32_t kWarmupOrders = 50000;
constexpr std::uint32_t kTargetResting = 50000;
constexpr int kThroughputRuns = 5;

// ---- workload ---------------------------------------------------------------------------------

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
};

struct Workload {
    std::vector<Event> warmup;  // builds the initial resting book (not measured)
    std::vector<Event> events;  // measured stream
};

// Order flow shaped like a liquid instrument: quotes cluster near the touch with a long tail,
// most orders are cancelled before they trade, and a minority of flow is aggressive.
static Workload make_workload(std::uint32_t n, std::uint64_t seed) {
    Rng r(seed);
    Workload w;
    Price mid = kTicks / 2;
    OrderId next_id = 1;
    std::vector<OrderId> live;  // candidates for cancel/modify (some may have filled already)
    auto depth = [&] { return 1 + static_cast<Price>(std::min(r.below(8), r.below(200))); };
    auto qty = [&] { return 1 + r.below(r.below(4) == 0 ? 500 : 50); };
    auto passive = [&](std::vector<Event>& out) {
        const Side s = r.below(2) ? Side::Buy : Side::Sell;
        const Price px = s == Side::Buy ? mid - depth() : mid + depth();
        out.push_back({EventType::Add, s, next_id, px, qty()});
        live.push_back(next_id++);
    };
    auto take_live = [&] {
        const std::uint32_t i = r.below(static_cast<std::uint32_t>(live.size()));
        const OrderId id = live[i];
        live[i] = live.back();
        live.pop_back();
        return id;
    };

    for (std::uint32_t i = 0; i < kWarmupOrders; ++i) passive(w.warmup);

    w.events.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (r.below(64) == 0) mid += r.below(2) ? 1 : -1;
        std::uint32_t roll = r.below(100);
        if (roll < 45 && live.size() > kTargetResting) roll = 50;  // steer the book size toward the target
        if (roll < 45 || live.empty()) {
            passive(w.events);
        } else if (roll < 50) {  // aggressive limit crossing a couple of ticks
            const Side s = r.below(2) ? Side::Buy : Side::Sell;
            const Price px = s == Side::Buy ? mid + 2 : mid - 2;
            w.events.push_back({EventType::Add, s, next_id, px, qty()});
            live.push_back(next_id++);
        } else if (roll < 85) {
            w.events.push_back({EventType::Cancel, Side::Buy, take_live(), 0, 0});
        } else if (roll < 95) {  // modify: mostly size-down (keeps priority), some reprices
            const OrderId id = live[r.below(static_cast<std::uint32_t>(live.size()))];
            const bool reprice = r.below(10) < 3;
            const Side s = r.below(2) ? Side::Buy : Side::Sell;  // side is ignored by modify
            const Price px = reprice ? mid + (r.below(2) ? depth() : -depth()) : -1;
            w.events.push_back({EventType::Modify, s, id, px, 1 + r.below(20)});
        } else {
            const Side s = r.below(2) ? Side::Buy : Side::Sell;
            w.events.push_back({EventType::Market, s, next_id++, 0, qty()});
        }
    }
    return w;
}

// ---- timing -----------------------------------------------------------------------------------

static inline std::uint64_t ticks() {
#ifdef TACHYON_HAVE_TSC
    _mm_lfence();
    const std::uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#else
    return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
}

static double calibrate_ticks_per_ns() {
    const auto c0 = Clock::now();
    const std::uint64_t t0 = ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::uint64_t t1 = ticks();
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - c0).count();
    return static_cast<double>(t1 - t0) / ns;
}

static std::string cpu_name() {
#ifdef TACHYON_HAVE_TSC
    unsigned regs[12] = {};
    unsigned max_ext = __get_cpuid_max(0x80000000u, nullptr);
    if (max_ext >= 0x80000004u) {
        for (unsigned i = 0; i < 3; ++i)
            __get_cpuid(0x80000002u + i, &regs[i * 4], &regs[i * 4 + 1], &regs[i * 4 + 2], &regs[i * 4 + 3]);
        std::string s(reinterpret_cast<const char*>(regs), 48);
        s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
#endif
    return "unknown";
}

static std::string compiler_name() {
#if defined(__clang__)
    return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    return "unknown";
#endif
}

struct CountingSink {
    std::uint64_t fills = 0, volume = 0;
    void on_fill(const Fill& f) noexcept {
        ++fills;
        volume += f.qty;
    }
};

static BookConfig bench_config(std::uint32_t max_id) {
    BookConfig c;
    c.num_ticks = kTicks;
    c.max_order_id = max_id;
    c.max_resting = 1u << 20;
    return c;
}

static OrderId max_id(const Workload& w) {
    OrderId m = 0;
    for (auto* v : {&w.warmup, &w.events})
        for (const Event& e : *v) m = std::max(m, e.id);
    return m + 1;
}

// Resolve size-only modifies against the stream itself (last known price of each id). Both books
// see identical resolved events, so this is fair and keeps the lookup out of the timed region.
static void resolve_modifies(Workload& w) {
    std::vector<Price> last(max_id(w), -1);
    for (auto* v : {&w.warmup, &w.events})
        for (Event& e : *v) {
            if (e.type == EventType::Add) last[e.id] = e.price;
            if (e.type == EventType::Modify) {
                if (e.price < 0) e.price = last[e.id] < 0 ? kTicks / 2 : last[e.id];
                last[e.id] = e.price;
            }
        }
}

struct Result {
    std::string name;
    double mops = 0;
    std::vector<double> runs_mops;
    std::vector<std::uint32_t> lat_ticks;  // per measured event
    CountingSink sink;
};

template <class Book>
static Result measure(const char* name, const Workload& w, OrderId ids) {
    Result res;
    res.name = name;
    // Throughput: no per-op timing.
    for (int run = 0; run < kThroughputRuns; ++run) {
        auto book = std::make_unique<Book>(bench_config(ids));
        CountingSink s;
        for (const Event& e : w.warmup) apply(*book, e, s);
        const auto t0 = Clock::now();
        for (const Event& e : w.events) apply(*book, e, s);
        const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
        res.runs_mops.push_back(static_cast<double>(w.events.size()) / sec / 1e6);
        res.sink = s;
    }
    auto sorted = res.runs_mops;
    std::sort(sorted.begin(), sorted.end());
    res.mops = sorted[sorted.size() / 2];
    // Latency: TSC around each operation.
    auto book = std::make_unique<Book>(bench_config(ids));
    CountingSink s;
    for (const Event& e : w.warmup) apply(*book, e, s);
    res.lat_ticks.resize(w.events.size());
    for (std::size_t i = 0; i < w.events.size(); ++i) {
        const std::uint64_t t0 = ticks();
        apply(*book, w.events[i], s);
        const std::uint64_t t1 = ticks();
        res.lat_ticks[i] = static_cast<std::uint32_t>(std::min<std::uint64_t>(t1 - t0, 0xFFFFFFFFu));
    }
    return res;
}

static std::uint32_t timer_overhead() {
    std::vector<std::uint32_t> v(200000);
    for (auto& x : v) {
        const std::uint64_t a = ticks();
        const std::uint64_t b = ticks();
        x = static_cast<std::uint32_t>(b - a);
    }
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

struct Pct {
    double p50, p90, p99, p999, max, mean;
};

static Pct percentiles(std::vector<double> ns) {
    std::sort(ns.begin(), ns.end());
    auto at = [&](double q) { return ns[std::min(ns.size() - 1, static_cast<std::size_t>(q * static_cast<double>(ns.size())))]; };
    double sum = 0;
    for (double x : ns) sum += x;
    return {at(0.50), at(0.90), at(0.99), at(0.999), ns.back(), sum / static_cast<double>(ns.size())};
}

static std::string pct_json(const Pct& p) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << "{\"p50\":" << p.p50 << ",\"p90\":" << p.p90 << ",\"p99\":" << p.p99 << ",\"p999\":" << p.p999
      << ",\"max\":" << p.max << ",\"mean\":" << p.mean << "}";
    return o.str();
}

int main(int argc, char** argv) {
    std::string out_path = "results/bench.json";
    std::uint32_t n = 5000000;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--out")) out_path = argv[i + 1];
        if (!std::strcmp(argv[i], "--events")) n = static_cast<std::uint32_t>(std::stoul(argv[i + 1]));
    }

    std::printf("generating %u events (+%u warmup orders)...\n", n, kWarmupOrders);
    Workload w = make_workload(n, 42);
    resolve_modifies(w);
    const OrderId ids = max_id(w);

    const double tpn = calibrate_ticks_per_ns();
    const std::uint32_t overhead = timer_overhead();
    const std::string cpu = cpu_name();
    std::printf("cpu: %s | tsc %.3f GHz | timer overhead %u ticks (%.1f ns, subtracted)\n\n", cpu.c_str(), tpn, overhead,
                overhead / tpn);

    Result results[] = {measure<OrderBook>("Tachyon", w, ids), measure<RefBook>("std::map reference", w, ids)};

    const bool agree = results[0].sink.fills == results[1].sink.fills && results[0].sink.volume == results[1].sink.volume;
    std::printf("fills: tachyon %llu / reference %llu, volume %llu / %llu -> %s\n\n",
                static_cast<unsigned long long>(results[0].sink.fills),
                static_cast<unsigned long long>(results[1].sink.fills),
                static_cast<unsigned long long>(results[0].sink.volume),
                static_cast<unsigned long long>(results[1].sink.volume), agree ? "IDENTICAL" : "MISMATCH");

    // Histogram edges: log-spaced from 4 ns to 64 us.
    std::vector<double> edges;
    for (double e = 4; e <= 65536; e *= 1.18) edges.push_back(e);

    static const char* kTypeNames[] = {"add", "market", "cancel", "modify"};
    std::ostringstream js;
    js.setf(std::ios::fixed);
    js.precision(3);
    js << "{\n\"machine\":{\"cpu\":\"" << cpu << "\",\"tsc_ghz\":" << tpn << ",\"timer_overhead_ns\":" << overhead / tpn
       << ",\"compiler\":\"" << compiler_name() << " -O3 -march=native\"},
"
       << "\"workload\":{\"events\":" << n << ",\"warmup_orders\":" << kWarmupOrders << ",\"ticks\":" << kTicks
       << ",\"target_resting\":" << kTargetResting
       << ",\"mix\":{\"passive_add\":45,\"aggressive_add\":5,\"cancel\":35,\"modify\":10,\"market\":5}},\n"
       << "\"fills_agree\":" << (agree ? "true" : "false") << ",\"fills\":" << results[0].sink.fills
       << ",\"volume\":" << results[0].sink.volume << ",\n\"results\":[";

    std::printf("%-22s %10s %9s %9s %9s %9s %10s\n", "book", "Mops/s", "p50 ns", "p90 ns", "p99 ns", "p99.9 ns", "max ns");
    Pct pcts[2];
    for (int k = 0; k < 2; ++k) {
        Result& r = results[k];
        std::vector<double> ns(r.lat_ticks.size());
        std::vector<std::vector<double>> by_type(4);
        for (std::size_t i = 0; i < ns.size(); ++i) {
            const double v = r.lat_ticks[i] > overhead ? (r.lat_ticks[i] - overhead) / tpn : 0.0;
            ns[i] = v;
            by_type[static_cast<int>(w.events[i].type)].push_back(v);
        }
        const Pct p = percentiles(ns);
        pcts[k] = p;
        std::printf("%-22s %10.2f %9.1f %9.1f %9.1f %9.1f %10.1f\n", r.name.c_str(), r.mops, p.p50, p.p90, p.p99, p.p999,
                    p.max);
        std::vector<std::uint64_t> hist(edges.size() + 1, 0);
        for (double v : ns) hist[std::upper_bound(edges.begin(), edges.end(), v) - edges.begin()]++;

        if (k) js << ",";
        js << "\n{\"name\":\"" << r.name << "\",\"mops\":" << r.mops << ",\"runs_mops\":[";
        for (std::size_t i = 0; i < r.runs_mops.size(); ++i) js << (i ? "," : "") << r.runs_mops[i];
        js << "],\"latency_ns\":" << pct_json(p) << ",\"by_type\":{";
        for (int t = 0; t < 4; ++t)
            js << (t ? "," : "") << "\"" << kTypeNames[t] << "\":" << pct_json(percentiles(by_type[t]));
        js << "},\"histogram\":[";
        for (std::size_t i = 0; i < hist.size(); ++i) js << (i ? "," : "") << hist[i];
        js << "]}";
    }
    js << "\n],\n\"histogram_edges_ns\":[";
    for (std::size_t i = 0; i < edges.size(); ++i) js << (i ? "," : "") << edges[i];
    js << "],\n\"speedup\":{\"throughput\":" << results[0].mops / results[1].mops
       << ",\"p50\":" << pcts[1].p50 / std::max(pcts[0].p50, 0.1) << ",\"p99\":" << pcts[1].p99 / std::max(pcts[0].p99, 0.1)
       << "}\n}\n";

    std::printf("\nspeedup vs reference: %.1fx throughput, %.1fx p99 latency\n", results[0].mops / results[1].mops,
                pcts[1].p99 / std::max(pcts[0].p99, 0.1));
    std::ofstream(out_path) << js.str();
    std::printf("wrote %s\n", out_path.c_str());
    return agree ? 0 : 1;
}
