# Data-Structure Benchmark: RB-tree vs Flat-Array Price Levels

> Milestone 3 — the "I made a *measured* engineering decision" story. This is
> the original contribution: not "I read that arrays are cache-friendly" but "I
> built both, ran the same 1M-order workload through each on my machine, and
> here is the number." Reproduce with `./build/bench_ds --orders 1000000`.

## The two designs

| | **OrderBook** (`order_book.h`) | **ArrayBook** (`array_book.h`) |
|---|---|---|
| price levels | `std::map<Price,Level>` (Red-Black tree) | `std::vector<Level>` indexed by `price - base` |
| best price | `begin()` — O(1), but a tree node | cursor index — O(1), a plain `long` |
| find level | O(log P) tree descent, pointer-chasing | O(1) direct index, contiguous memory |
| new level | `operator new` for a tree node | already allocated (just stamp it) |
| memory | only occupied levels | the whole `[min,max]` band, empty or not |
| price range | unbounded / sparse-friendly | must be bounded & known up front |
| out-of-band price | always accepted | **rejected** (acts like an exchange price band) |

Both share the *identical* order storage: an intrusive doubly-linked FIFO list
per level + an `ObjectPool<Order>` + an `unordered_map<id,node>` for O(1) cancel.
So this benchmark isolates exactly one variable: **the price-level container.**

## Correctness first (Section 11 cross-check)

Before comparing speed, `bench_ds` feeds the same command stream to both and
compares a 64-bit FNV hash of every trade plus the serialized final book:

```
CROSS-CHECK (RB-tree vs flat-array on identical input):
  trades:     IDENTICAL  (count=728625 vol=18596012, hashes equal)
  final book: IDENTICAL
```

Identical across every seed tried (1, 7, 42, 0xC0FFEE). A fast wrong engine is
worthless (Section 11) — this is the proof the array implementation matches the
reference tree exactly.

## The numbers (i5-12450H, MSVC /O2, this machine — yours will differ)

Throughput (M orders/sec) and per-op latency (ns), several seeds:

| seed | orders | RB-tree M/s | array M/s | speedup | RB p50 | array p50 |
|---:|---:|---:|---:|---:|---:|---:|
| C0FFEE | 1,000,000 | 4.48 | 4.71 | 1.05× | 178 | 162 |
| 1 | 800,000 | 4.63 | 6.21 | **1.34×** | 162 | 130 |
| 7 | 800,000 | 5.76 | 6.11 | 1.06× | 140 | 130 |
| 42 | 800,000 | 5.47 | 6.52 | 1.19× | 147 | 126 |

**The flat array wins on every run** — ~5–34% on throughput and consistently
lower p50/p90 latency. It also has lower mean. (Run-to-run `max` is noisy on a
laptop and not a basis for comparison; see `bench.md`.)

## Why the array wins *here*

1. **O(1) vs O(log P) level access.** The hot op is "find the best/this price
   level." The array is one indexed load; the tree is a `log P` descent.
2. **Cache locality.** Array levels are contiguous — walking the book near the
   touch stays in a few cache lines. RB-tree nodes are scattered on the heap, so
   each hop is a likely cache miss (pointer-chasing).
3. **No per-level allocation.** `std::map` calls `operator new` for every new
   price level; that is the source of the tree's ~hundreds-of-µs `max` spikes in
   `bench.md`. The array pre-allocates the whole band once.

The workload's price band here is dense (~490 ticks around the mid) — the
array's best case.

## Why the array does *not* always win (the honest caveat)

- It needs a **bounded, known price range**. We auto-size it to the data's band;
  a real venue uses its tick table / price bands. A price outside the band is
  rejected (we count `rejected`), which is correct venue behaviour but means the
  array can't represent an arbitrarily wide book.
- It **wastes memory on empty ticks**. For a sparse or very wide book (e.g. a
  low-priced crypto pair quoted across millions of ticks), the array would waste
  huge memory and thrash the TLB, and the tree — which stores only occupied
  levels — would win. The crossover is when the band is large relative to the
  number of *occupied* levels.

## Conclusion / what a real engine does

On a dense book around the touch, the flat array is the better choice and the
benchmark backs that with a real, reproducible number. Production HFT engines
take the **hybrid**: a flat array for the dense region near the best price where
all the action is, and a tree (or hash) for the sparse tails — getting the
array's cache behaviour where it matters without its memory blow-up in the tails.
That hybrid is the natural next step from this measurement.
