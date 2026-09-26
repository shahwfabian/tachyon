# Tachyon

**A C++20 limit order book that is fast, and can prove it's correct.**

Tachyon is a single-instrument, price-time-priority matching engine, plus the tooling that keeps it honest:

- **Latency benchmark.** The engine is measured per operation with the CPU's timestamp counter against a `std::map` reference book, replaying the same order stream from the same pre-built book.
- **Differential fuzzer.** Random order flow runs through Tachyon and the reference book side by side. After every event, both books must report the same status, the same fills and the same full book state, and Tachyon's own invariants must still hold.
- **Test-case shrinker.** When a check fails, a delta-debugging shrinker cuts the failing sequence down to a minimal reproduction, usually 2–4 events.
- **Planted-bug campaign.** Eight realistic matching-engine defects are compiled in behind a flag. The fuzzer has to find and shrink every one of them.

**Live demo: [tachyon-lob.vercel.app](https://tachyon-lob.vercel.app).** The site runs the actual C++ engine in your browser, compiled to a 27 KB WebAssembly module, and includes a step-through viewer for every shrunk failure.

---

## Results

### Latency and throughput

5,000,000 operations over a book of about 50k resting orders, on a single core of a laptop Intel i7-8650U (clang 21, `-O3 -march=native`):

| | **Tachyon** | std::map reference | speedup |
|---|---:|---:|---:|
| throughput | **15.2M ops/s** | 3.6M ops/s | **4.3×** |
| p50 latency | **81 ns** | 229 ns | 2.8× |
| p90 latency | **205 ns** | 617 ns | 3.0× |
| p99 latency | **538 ns** | 1,927 ns | **3.6×** |
| p99.9 latency | **1.49 µs** | 4.81 µs | 3.2× |

Both books produced **identical** output: 1,903,586 fills totalling 76,265,894 lots. The benchmark fails if they disagree.

### Planted-bug campaign

| Planted bug | Caught by | Failing sequence → minimal repro |
|---|---|---:|
| Dust fill (1-lot remainder silently dropped) | book diff | 21 → **2** events |
| Trade-through (fills one tick past the limit) | fill diff | 35 → **3** |
| Stale best price after cancel | invariant | 41 → **2** |
| Level quantity leak on cancel | invariant | 30 → **3** |
| Size-up keeps time priority | book diff | 29 → **4** |
| LIFO on second order at a level | book diff | 24 → **2** |
| Bitmap word-boundary scan | invariant | 101 → **3** |
| Market-order remainder rests | trading rule | 3 → **2** |

**8/8 found and shrunk. Zero false positives** across 10,000,000 events on the clean engine.

Example of a shrunk failure: the bitmap scan misses a level sitting on bit 63 of a 64-bit word.

```
1. ADD #1 SELL 1 @ 447        <- 447 = 6*64 + 63, the last bit of a word
2. ADD #2 SELL 1 @ 446
3. MKT #3 BUY 1               <- clears 446; the scan for the next best ask skips 447
✗ invariant: cached best ask 1024 (none) != bitmap best ask 447
```

---

## Design

```
bid bitmap   |....1...1.1......|      1 bit per tick; next best price = countr_zero / countl_zero
ask bitmap   |............1..1.|
price levels [ head | tail | qty | count ] x num_ticks     flat array, O(1) by tick
order pool   [ #17 ] <-> [ #42 ] <-> [ #88 ]               32-bit intrusive links, free list
id index     id -> pool slot                               O(1) cancel / modify
```

- **Integer ticks, one shared ladder.** An uncrossed book can never hold a bid and an ask at the same tick, so both sides share a single array of price levels.
- **Cached best price plus bitmap scan.** The best bid and ask are updated as orders arrive. The bitmap is scanned only when the best level empties, 64 ticks per word checked.
- **Intrusive FIFO queues.** Orders sit in preallocated slots, so there's no allocation on the hot path. Freed slots are reused most-recent-first, which keeps recently used memory in cache.
- **Exchange semantics:**
  - trades execute at the resting order's price;
  - reducing an order's size keeps its time priority;
  - increasing its size or changing its price loses priority;
  - market orders are immediate-or-cancel.
- **Zero-cost bug hooks.** `TACHYON_BUG(...)` compiles to `false` unless `TACHYON_PLANTED_BUGS` is defined. Only the fuzz target defines it.

### How the fuzzer works

1. **Generate.** A seeded generator produces order flow around a random-walking mid price: passive and aggressive limit orders, market orders, cancels and modifies aimed at earlier orders. Quantities lean small, to hit edge cases.
2. **Run in lockstep.** The same flow runs through `OrderBook` and `RefBook`, a plain `std::map<price, std::list>` implementation that's easy to check by eye. After each event it checks:
   - Tachyon's structural invariants: bitmaps, links, counts, aggregate quantities, cached best prices, and that the book is never crossed;
   - trading rules that need no reference: no trade-through, no overfill, market orders never rest;
   - that status, fills and the full book (every level, and every order in queue order) match the reference exactly.
3. **Shrink.** Delete chunks of events at halving sizes (delta debugging), then simplify quantities and prices. Repeat until no change helps, keeping only candidates that fail the same way as the original. Finally, renumber order ids 1..k so the repro is easy to read.

---

## Build and run

The project has no dependencies. You need a C++20 compiler.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/tachyon_tests                    # 15 unit tests
./build/tachyon_fuzz --out fuzz.json     # clean campaign + planted-bug campaign
./build/tachyon_bench --out bench.json   # latency / throughput vs std::map
```

On a machine without a system compiler (e.g. Windows):

```bash
pip install ziglang
python build.py all     # tests, fuzz, bench, wasm, then refresh web/data
```

CI (`.github/workflows/ci.yml`) runs the tests and the fuzz campaign on GCC and Clang (Linux), Clang (macOS), and an AddressSanitizer + UndefinedBehaviorSanitizer build.

### Layout

```
include/tachyon/order_book.hpp   the engine (header-only)
include/tachyon/ref_book.hpp     reference implementation for differential testing
include/tachyon/events.hpp       shared event format
tests/tests.cpp                  unit tests
fuzz/fuzz.cpp                    generator, lockstep runner, shrinker, trace export
bench/bench.cpp                  TSC latency + throughput benchmark
wasm/tachyon_wasm.cpp            C ABI for the browser build
web/                             static site (deployed on Vercel)
```

## Methodology notes

- Latency is measured with `lfence; rdtsc` around each call. The timer's own overhead (median of back-to-back reads, about 20 ns) is subtracted. The TSC is calibrated against `steady_clock`.
- The workload is synthetic. Its order mix is 45% passive limits, 5% aggressive limits, 35% cancels, 10% modifies and 5% market orders. Quotes cluster near the touch with a long tail, and there are about 50k resting orders. Cancels target random live orders, so most operations cost at least one cache miss. That's realistic, and it sets the floor for the median.
- Throughput is the median of 5 runs. The run doesn't pin threads or tune the OS, and a laptop has frequency scaling, so treat the numbers as conservative.
- The planted bugs are known defects, so catching them shows the harness is sensitive, not that the engine is bug-free. Evidence for the clean engine comes from the unit tests plus 10M fuzzed events with zero divergences from the reference.
