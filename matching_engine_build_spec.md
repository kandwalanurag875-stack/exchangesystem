# BUILD SPEC & LLM PROMPT — Fault-Tolerant Low-Latency Matching Engine ("Mini-Exchange")

> **How to use this document.** This is a complete specification you can paste into an LLM coding assistant (Claude Code, Cursor, ChatGPT, etc.) at the start of a session, or read yourself as a build guide. It assumes you are starting from zero. Don't read all 15 pages before coding — read Section 0–3 once for context, then pull the relevant component section when you reach that milestone. Build in the order given in Section 12.

---

## SECTION 0 — INSTRUCTIONS TO THE LLM (read this first)

You are an expert systems-and-trading-infrastructure engineer mentoring a strong 3rd-year CS undergraduate. The student is a competitive programmer (comfortable with C++, data structures, algorithms) who has already built a C++ NoSQL key-value store (Robin-Hood hashing, Bloom filters, segmented on-disk storage, a thread-safe REST API). So: assume fluency in C++, pointers, templates, STL, and basic concurrency. Do NOT over-explain beginner concepts. DO explain anything specific to low-latency engineering, market microstructure, lock-free concurrency, and crash-recovery, because those are new to the student.

Your job across the session:
1. Help the student build the project described below, **one milestone at a time** (see Section 12). Never dump the whole codebase at once. Finish and test one milestone before moving on.
2. For every non-trivial design choice, explain the **tradeoff** (why this data structure, why this memory layout) — because the student must be able to defend every decision in an interview.
3. Insist on **measurement**. Every performance claim must be backed by a benchmark the student ran, with a methodology. Refuse to let the student write "it's fast" without a number.
4. Keep the code **idiomatic, well-commented, and testable**. Prefer clarity over cleverness except on the documented hot path.
5. When the student is stuck, give a hint and the relevant concept first; give full code only when asked or when the student has tried.

Constraints:
- Primary language: **C++17 or C++20** for the engine (this is a low-latency project; the student's quant target requires C++). Use **Python** only for the benchmark plotting and the optional web visualizer backend glue.
- No external heavyweight frameworks for the core engine. Standard library + minimal dependencies only. The point is to show the student built the hard parts, not glued libraries together.
- Everything must compile and run on a normal Linux laptop (Ubuntu). No exotic hardware required.

---

## SECTION 1 — WHAT WE ARE BUILDING (one paragraph)

We are building a **matching engine** — the core software inside a stock exchange (like NSE, NASDAQ, or a crypto exchange). It maintains an **order book** (all the pending buy and sell orders), and when a new order arrives, it **matches** it against the opposite side using price-time priority and produces **trades**. On top of the matching core, we add two things: (a) a **low-latency, lock-free pipeline** so the engine processes orders at microsecond/nanosecond speed (this is the *quant* story), and (b) a **durable, replayable command log** so the engine survives crashes without losing or corrupting data (this is the *distributed-systems / backend* story). We also build a **benchmark harness with latency histograms** (Level 2) and a **live web visualizer of the order book** (Level 3).

This single project tells two complete stories to two different kinds of employer. See Section 2.

---

## SECTION 2 — WHY THIS PROJECT (the dual value — keep this in mind throughout)

A real exchange is, simultaneously, a low-latency trading system AND a fault-tolerant distributed system. So this project is not "bent" to fit two job descriptions — it naturally sits in the intersection.

**For QUANT / HFT firms (DE Shaw, QuadEye, NK Securities, Jane Street, Optiver-style):**
- The order book + matching logic proves the student understands *market microstructure* (price-time priority, order types, the spread).
- The nanosecond-level optimization (cache efficiency, pre-allocation, lock-free ring buffer) proves the student can write genuinely fast C++ — the single most-tested skill at these firms.
- The benchmark discipline (p50/p99/p99.9 latency, throughput) is exactly how these firms measure their own systems.

**For SYSTEMS / BACKEND firms (Stripe, Rubrik, Confluent, Google):**
- Strip away the trading vocabulary and the engine is a *deterministic state machine fed by a replicated command log* — which is the architecture of etcd, Kafka, and every payment ledger.
- The write-ahead log + crash recovery proves the student understands *durability* and *consistency*.
- Stripe in particular runs money-movement as an idempotent, single-writer ledger — this project mirrors that.
- The single-writer + event-sourcing design is a recognized, respected pattern (it is literally the LMAX architecture Martin Fowler wrote about).

**The mental model:** Layer 1 = "I understand exchanges." Layer 2 = "I can make it fast." Layer 3 (and the durability layer) = "I can make it survive failure." The student should be able to switch which story they lead with depending on who they're talking to — same code, different emphasis.

---

## SECTION 3 — SOURCE PAPERS & REFERENCE REPOSITORIES (what to draw from)

The student works best by reading a paper, understanding the idea, then building their own implementation and measuring it. Honor that. The student must **read and understand these, then build their own version** — NOT clone and submit. The originality is in *their* implementation choices, *their* benchmarks, and *their* design decisions.

### Primary papers
1. **LMAX Disruptor — "High performance alternative to bounded queues for exchanging data between concurrent threads"** (Thompson, Farley, Barker, Gee, Stewart). The foundational whitepaper from the LMAX financial exchange. Describes the lock-free ring buffer that lets a single thread process ~25 million messages/sec at <50 nanosecond latency. This is the conceptual basis for Layer 2 (the low-latency pipeline). URL: https://lmax-exchange.github.io/disruptor/disruptor.html (PDF: https://lmax-exchange.github.io/disruptor/files/Disruptor-1.0.pdf)
   - Also read Martin Fowler's architecture write-up: https://martinfowler.com/articles/lmax.html
   - And Trisha Gee's "Dissecting the Disruptor" blog series for an approachable explanation of the ring buffer.

2. **CoinTossX — "An open-source low-latency high-throughput matching engine"** (Jericevich, Sing & Gebbie, 2021). arXiv:2102.10925. A full academic matching engine with the design rationale spelled out. Key takeaway to internalize: active orders are stored *in memory* (not on disk) because disk I/O would kill latency, and the order-book data structure is chosen for low memory overhead and CPU-cache efficiency. Use this as the *reference architecture*. Paper: https://arxiv.org/abs/2102.10925 — Code: https://github.com/dharmeshsing/CoinTossX

3. **"C++ Design Patterns for Low-latency Applications Including High-frequency Trading"** (arXiv:2309.04259). Translates the Disruptor's Java ideas into modern C++ — cache-line padding, compare-and-swap, pre-allocation, avoiding false sharing. Since the engine is in C++, this bridges the Java papers to the student's language. Paper: https://arxiv.org/abs/2309.04259

### Reference repositories (study the structure and ideas, then build your own)
- **brprojects/Limit-Order-Book** (C++) — clean, well-documented, ~1.4M tps. Best for understanding the *binary-tree-of-price-levels + doubly-linked-list-of-orders + hashmap* structure. https://github.com/brprojects/Limit-Order-Book
- **Johannestampere/order_book_simulator** (C++20) — core matching operations in 8–10 nanoseconds. Good for seeing aggressive optimization and cache-locality techniques. https://github.com/Johannestampere/order_book_simulator
- **AnuragBand-07/low-latency-order-book** (C++) — clearest explanation of the Red-Black-tree (std::map) + doubly-linked-list + hashmap layering for O(1) cancels. https://github.com/AnuragBand-07/low-latency-order-book
- **dharmeshsing/CoinTossX** (Java) — the full implementation behind paper #2. Reference for the overall system shape, not the language.

### Reference repositories for the durability / replication layer (the systems story)
- **debajyotidasgupta/raft-consensus** (Go) — clean, course-built Raft implementation, good for understanding leader election + log replication. https://github.com/debajyotidasgupta/raft-consensus
- **skyzh/mini-lsm** (Rust) — excellent for understanding write-ahead-log + crash recovery patterns (the WAL, fsync discipline, replay). https://github.com/skyzh/mini-lsm

### Data sources (for realistic benchmarks)
- **LOBSTER** sample data (reconstructed limit order book data from NASDAQ ITCH) — https://lobsterdata.com (free sample files available).
- **NASDAQ ITCH** specification / sample feeds — for the stretch goal of parsing a real exchange protocol.
- If real data is hard to obtain initially, generate synthetic orders: prices ~ Normal(centered on a mid price), a realistic mix of ~80% add/cancel and ~20% executing orders (CoinTossX and the reference repos describe this distribution).

---

## SECTION 4 — CORE CONCEPTS THE STUDENT MUST UNDERSTAND (explain these as they come up)

### 4.1 The order book
Two sorted collections of resting (unmatched) orders:
- **Bids** (buy orders): sorted by price *descending* — the highest price a buyer will pay is the "best bid" and sits at the top.
- **Asks** (sell orders): sorted by price *ascending* — the lowest price a seller will accept is the "best ask" and sits at the top.
- The gap between best bid and best ask is the **spread**. When they cross (a buyer willing to pay ≥ what a seller will accept), a **trade** happens.

### 4.2 Price-time priority (the matching rule)
When a new order can match, it matches against the best price first. Among orders at the *same* price, the one that arrived *earliest* matches first (FIFO). This is the standard rule at most exchanges and is what we implement.

### 4.3 Order types (implement in this order of priority)
- **Limit order**: "buy/sell up to N shares at price P or better." If it can match immediately it does; the unmatched remainder rests in the book.
- **Market order**: "buy/sell N shares at whatever the best available price is." Never rests; matches until filled or book is empty. (Common implementation trick: treat a market buy as a limit buy at +infinity, a market sell as a limit sell at −infinity, so the same matching code handles both.)
- **Cancel**: remove a resting order by its ID.
- **Modify** (optional): change quantity/price of a resting order (often implemented as cancel + re-add).
- **Advanced flags** (stretch): IOC (Immediate-Or-Cancel), FOK (Fill-Or-Kill), POST_ONLY, STP (self-trade prevention).

### 4.4 Why in-memory, not on disk
The active order book lives entirely in RAM. Disk is only touched by the durability log (Section 8), and even that is appended sequentially (fast), never random-access. This is the single most important latency decision and the student must be able to explain it.

### 4.5 Determinism (critical for the systems story)
Given the same sequence of input commands, the engine must always produce the *exact same* sequence of trades and the *exact same* final book state. This is what makes crash recovery possible: replay the log → identical state. No randomness, no wall-clock-dependent behavior in the matching path.

---

## SECTION 5 — ARCHITECTURE OVERVIEW

```
                 ┌──────────────────────────────────────────────────┐
                 │                  MINI-EXCHANGE                     │
                 │                                                    │
  Orders  ──────▶│  [Ingress]  ──▶  [Ring Buffer]  ──▶ [Single-Writer │
  (CSV / feed /  │                  (lock-free,        Matching       │
   web / synthetic)                  pre-allocated)    Engine]        │
                 │                        │                │          │
                 │                        ▼                ▼          │
                 │                  [WAL / Command    [Order Book]    │
                 │                   Log on disk]     (bids+asks,     │
                 │                        │            in memory)     │
                 │                        ▼                │          │
                 │                  [Replica(s) /          ▼          │
                 │                   crash recovery]   [Trades out]   │
                 │                                          │         │
                 └──────────────────────────────────────────┼────────┘
                                                             ▼
                                          [Benchmark harness + latency CSV]   (Level 2)
                                          [Web visualizer of live order book]  (Level 3)
```

**Layer 1 (correctness):** Order book + matching engine. A program that reads orders and emits trades.
**Layer 2 (speed):** The ring-buffer pipeline + single-writer thread + the benchmark + latency histogram.
**Layer 3 (durability/systems):** The write-ahead log + crash recovery + (stretch) replication.
**Presentation:** Benchmark plots (Level 2 deliverable) + live web order-book visualizer (Level 3 deliverable).

---

## SECTION 6 — COMPONENT SPEC: THE MATCHING ENGINE (Layer 1)

### 6.1 Data structures (and the tradeoff to discuss)
The classic, interview-friendly design (used by brprojects and AnuragBand repos):

- **Price levels:** a balanced BST keyed by price. In C++, `std::map<Price, PriceLevel>` (a Red-Black tree) gives sorted iteration and O(log P) insert/find where P = number of distinct price levels. Best bid = `bids.rbegin()`, best ask = `asks.begin()`.
- **Orders at a price level:** a **doubly-linked list** (FIFO queue) of orders. New orders append to the tail; matching consumes from the head. DLL gives O(1) removal from the middle (for cancels).
- **Order lookup:** a `std::unordered_map<OrderId, OrderNode*>` mapping each order ID to its node *inside* the DLL, so **cancel is O(1)** — you jump straight to the node and unlink it.

**The tradeoff the student MUST be able to explain:**
- `std::map` (Red-Black tree) vs a **flat array / `std::vector` indexed by price tick**: the flat array gives O(1) best-price access and better cache locality but wastes memory if the price range is huge and is awkward for sparse books. The RB-tree is memory-efficient for sparse books but has pointer-chasing (cache misses). **The student should benchmark BOTH and report which wins on their workload.** This benchmark *is* their original contribution. (Real HFT engines often use a hybrid: a flat array for the dense region near the touch, a tree for the tails.)

### 6.2 Core operations and their target complexity
- `addLimitOrder(side, price, qty, id)` → match against opposite side while crossing; rest the remainder. O(log P) to find level + O(matches).
- `addMarketOrder(side, qty)` → match against opposite side until filled or empty.
- `cancelOrder(id)` → O(1) via the hashmap.
- `modifyOrder(id, newQty, newPrice)` → cancel + re-add (loses time priority, which is correct exchange behavior on a price change).
- `bestBid()` / `bestAsk()` → O(1).

### 6.3 The matching loop (pseudocode — student implements in C++)
```
function addLimitOrder(side, price, qty, id):
    if side == BUY:
        while qty > 0 and asks not empty and asks.bestPrice <= price:
            level = asks.bestLevel()
            headOrder = level.front()
            tradedQty = min(qty, headOrder.qty)
            emitTrade(buyer=id, seller=headOrder.id, price=level.price, qty=tradedQty)
            qty -= tradedQty
            headOrder.qty -= tradedQty
            if headOrder.qty == 0:
                level.popFront(); removeFromLookup(headOrder.id)
                if level empty: asks.removeLevel(level.price)
        if qty > 0:
            restOrder(BUY, price, qty, id)   # add to bids, record in lookup
    else:
        # symmetric for SELL against bids
```
Emphasize: a trade always executes at the **resting** order's price (the order already in the book sets the price), never the incoming order's price. This is a subtle correctness point interviewers probe.

### 6.4 Correctness invariants (turn each into a unit test — Section 11)
- Bids are always sorted descending, asks ascending.
- Best bid < best ask whenever both sides non-empty (book never stays crossed after matching).
- Total shares conserved: shares added − shares matched − shares cancelled = shares resting.
- An order ID in the lookup map exists in exactly one price level's DLL.
- Replaying the same command sequence twice yields identical trades and identical final book.

---

## SECTION 7 — COMPONENT SPEC: THE LOW-LATENCY PIPELINE (Layer 2)

### 7.1 The Disruptor / ring-buffer pattern (from paper #1)
Instead of passing orders between threads via a lock-based queue (`std::mutex` + `std::queue`), use a **pre-allocated bounded ring buffer**:
- A fixed-size array (size = power of 2, so index wrap = bitwise AND with size−1 — no modulo).
- All slots allocated **once** at startup; never allocate on the hot path (no `new`/`malloc` during processing — this is the cardinal rule of low-latency).
- **Producer(s)** write orders into slots; a **single consumer** (the matching thread) reads them in order.
- Coordination via atomic **sequence counters** (each producer/consumer publishes its position; others read it) using `std::atomic` with carefully chosen `memory_order` — NOT mutexes.
- **Single-writer principle:** only ONE thread mutates the order book. This eliminates locks on the book entirely and makes the system deterministic. Producers can be many; the matching writer is one.

### 7.2 Mechanical sympathy details to implement and explain
- **Cache-line padding** to prevent *false sharing*: pad the producer and consumer sequence counters so they don't share a 64-byte cache line. (`alignas(64)`.)
- **Pre-allocation / object pools:** order objects are reused from a pool, never allocated per-order.
- Compile flags for the hot path: `-O3 -march=native`, and for the engine `-fno-exceptions -fno-rtti` where appropriate (the reference repos do this).
- Avoid branches on the hot path where possible; consider side-specialized matching (separate code paths for buy vs sell) to help the branch predictor.

### 7.3 What "fast" must mean here (be honest)
A from-scratch student engine will NOT hit the LMAX 50ns figure, and the LLM should not let the student claim it. Realistic, defensible targets on a laptop:
- Core match operation: **single-digit to low-double-digit microseconds**, ideally sub-microsecond for the hot path on simple adds/cancels if well-optimized (the strong reference repos reach hundreds of nanoseconds to single-digit microseconds).
- Throughput: **hundreds of thousands to a few million orders/sec** single-threaded.
- The exact number matters less than the **methodology and the honesty**. A well-measured 800k orders/sec beats a fabricated 20M.

---

## SECTION 8 — COMPONENT SPEC: DURABILITY & RECOVERY (Layer 3 — the systems story)

### 8.1 Write-ahead log (WAL)
Before the matching engine applies a command, the command is **appended to a log file on disk** and **fsync'd** (forced to physical storage). Only after the durable write does the engine acknowledge and apply it. This is the same WAL discipline used by databases (and by the student's own DynamicKV on-disk layer — draw the parallel).
- Log format: a compact binary record per command (sequence number, type, side, price, qty, order id, timestamp). Append-only, sequential writes (fast).
- Each record gets a monotonically increasing **sequence number**.

### 8.2 Crash recovery
On startup, the engine **replays the WAL** from the beginning (or from the last snapshot), re-applying every command in order. Because matching is deterministic (Section 4.5), the rebuilt book and trade history are *identical* to before the crash. Demonstrate this: kill the process mid-run, restart, show the book is restored exactly.

### 8.3 Snapshots (optimization)
Replaying a huge log is slow. Periodically write a **snapshot** of the full book state + the sequence number it corresponds to. On recovery: load latest snapshot, then replay only the log records after it. (Same idea as Raft log compaction / LSM checkpoints.)

### 8.4 Replication (STRETCH GOAL — only if time allows; biggest systems payoff)
Run the engine as a small cluster: one **primary** (the single writer) and one or more **backups**. The primary replicates its command log to backups before acknowledging. If the primary crashes, a backup takes over with an identical log. The clean way is to put the command log under **Raft** (study debajyotidasgupta/raft-consensus). This turns the project into "a replicated, fault-tolerant exchange" — the strongest possible systems statement. Treat as optional; Layers 1–3 without replication already tell a strong story.

---

## SECTION 9 — DELIVERABLE: BENCHMARK HARNESS + LATENCY HISTOGRAM (Level 2)

This is non-negotiable for *both* audiences. No benchmark = a toy.

### 9.1 What to measure
- **Latency per operation:** wrap each `addOrder` call with a high-resolution timer (`std::chrono::high_resolution_clock` or `rdtsc` for finer granularity). Record the nanosecond cost of each operation into a pre-allocated array (do NOT print or allocate during measurement — that pollutes the numbers).
- **Throughput:** total orders processed / total wall-clock time.
- Run on a large input (≥1,000,000 orders) — synthetic or LOBSTER-replayed.

### 9.2 What to report (the percentiles, not the average)
Averages lie. Report the distribution:
- **p50** (median), **p90**, **p99**, **p99.9**, and **max** latency.
- Throughput in orders/sec.
- Separate the numbers by operation type (add vs cancel vs match) — they have very different costs.

### 9.3 The histogram (the visual artifact)
After the run, dump the per-operation latencies to a CSV. A short Python script (matplotlib) reads the CSV and produces:
- A **latency histogram** (x = latency bucket in nanoseconds/microseconds, y = count). Use a log scale on the y-axis (the strong repos do — most ops are fast, a long tail is rare).
- A **throughput-vs-book-depth** plot (optional): how latency degrades as the book grows.

### 9.4 Methodology doc (`docs/bench.md`)
Write down: the machine (CPU, RAM, OS), compiler + flags, input data and size, warm-up procedure, how latency was timed, and the raw percentile table. This methodology section is what separates a serious engineer from someone who pasted a number. Interviewers respect it enormously.

### 9.5 Expected output (example shape — student fills in real numbers)
```
$ ./bench --orders 1000000 --data synthetic

Engine:    single-writer, RB-tree price levels
Machine:   Intel i5-12450H, 16GB, Ubuntu 24.04, g++ -O3 -march=native
Orders:    1,000,000  (78% add, 14% cancel, 8% market)
Trades:    341,902
Throughput: 1.9 M orders/sec
Latency (ns):  p50=420  p90=610  p99=1180  p99.9=4300  max=22100
CSV written:  docs/bench.csv   →  python plot_hist.py  →  docs/latency_hist.png
```

---

## SECTION 10 — DELIVERABLE: LIVE VISUAL ORDER BOOK (Level 3)

A web page that shows the order book updating in real time as orders flow through the engine. This is the "show me something" artifact and reuses the student's web skills (Next.js/React/TypeScript from their existing projects).

### 10.1 Architecture
- The C++ engine exposes its current book state over a lightweight channel: simplest is the engine writes book snapshots (top N levels) to a **WebSocket** or to a small local HTTP endpoint at a fixed interval (e.g., every 50–100ms — this is for *human viewing*, not the hot path, so it must NOT touch the latency-critical thread; have a separate low-priority thread read a consistent snapshot).
- A **React/Next.js** front end connects, receives book snapshots, and renders.

### 10.2 What it shows
```
        ASKS (sellers)
   1402  ████████        50
   1401  ████            30
   1400  ██              20      ← best ask
   ─────────────────────────  spread = 0
   1400  █               15      ← best bid
   1399  ████████        40
   1398  █████           25
        BIDS (buyers)

   Last trade: 15 @ 1400   |   Trades/sec: 3,210   |   Spread: 0
```
- Horizontal bars sized by resting quantity at each price (depth visualization).
- Bids green, asks red; the spread highlighted in the middle.
- A live ticker: last trade price/qty, trades-per-second, current spread.
- Optional: a scrolling trade tape (recent executions) and a tiny price chart of the mid-price over time.

### 10.3 Important honesty caveat (the LLM must tell the student)
This visualizer is a **presentation aid**, not a feature of a real HFT system — real exchanges have no human-facing UI on the hot path; everything is automated and headless. So: build it for demos and to reuse web skills, but in interviews present it as "a visualization layer I added on top, deliberately off the latency-critical path," NOT as a core engineering achievement. The engine + benchmark is the substance; the visualizer is the showcase.

### 10.4 Keep it off the hot path (critical correctness point)
The matching thread must never block on the visualizer. Use a separate thread that copies a consistent snapshot of the book (or reads a lock-free published snapshot) and serves it to the browser. If the web layer is slow or disconnects, the engine keeps running at full speed. Be ready to explain this isolation — it demonstrates you understand separating the critical path from observability.

---

## SECTION 11 — TESTING STRATEGY (correctness before speed)

The LLM must push the student to write these BEFORE optimizing. A fast wrong engine is worthless.

- **Unit tests** (use Catch2 or GoogleTest): each order type, each edge case — empty book, partial fill, full fill, cancel of non-existent order, market order against empty book, self-cross.
- **Invariant checks** (Section 6.4): after every operation in a test run, assert the book invariants hold.
- **Determinism test:** run a fixed command sequence twice; assert identical trade output and identical final book serialization. This also validates crash recovery.
- **Recovery test:** process N orders, snapshot/log, simulate a crash (kill + restart), replay, assert the recovered book equals the pre-crash book exactly.
- **Property/fuzz test (stretch):** generate random valid command sequences, run them, assert invariants never break.
- **Reference cross-check (stretch):** feed the same LOBSTER input to your engine and to a simple, obviously-correct (slow) reference implementation; assert the trade outputs match.

---

## SECTION 12 — BUILD PLAN / MILESTONES (build in THIS order)

Each milestone is independently demoable. Stop after any milestone and you still have something real.

**Milestone 1 — Correct single-threaded matching engine (Layer 1).**
Order book with `std::map` + DLL + lookup hashmap. Limit + market + cancel. Reads orders from a CSV, prints trades. Unit tests + invariants pass. *Demoable: "here's a working exchange core."* (Est. 2–3 weeks.)

**Milestone 2 — Benchmark + latency histogram (Level 2).**
Add timing, run 1M+ orders, produce percentile table + CSV + matplotlib histogram + `docs/bench.md`. *Demoable: real performance numbers.* (Est. 1 week.)

**Milestone 3 — Data-structure benchmark (your original contribution).**
Implement the flat-array price-level alternative; benchmark it against the RB-tree on the same workload; document which wins and why. *This is the "I made a measured engineering decision" story.* (Est. 1 week.)

**Milestone 4 — Low-latency pipeline (Layer 2 deepening).**
Introduce the pre-allocated ring buffer + single-writer thread + object pool + cache-line padding. Re-benchmark; show the improvement vs Milestone 1. *Demoable: "I applied the LMAX Disruptor pattern and here's the before/after."* (Est. 2 weeks.)

**Milestone 5 — Durability: WAL + crash recovery (Layer 3).**
Append-only fsync'd command log; replay on startup; snapshots. Recovery test passes. *Demoable: "kill it mid-run, it recovers exactly."* (Est. 1–2 weeks.)

**Milestone 6 — Live web visualizer (Level 3).**
WebSocket snapshot feed off the hot path + React/Next.js depth visualization + trade ticker. *Demoable: the cool live order book.* (Est. 1 week.)

**Milestone 7 (STRETCH) — Replication via Raft.**
Primary + backup, replicated command log, failover. *Demoable: "kill the primary, a backup takes over with no data loss."* (Open-ended.)

**Milestone 8 (STRETCH) — Real market-data feed handler.**
Parse ITCH/LOBSTER messages and reconstruct the book from a real exchange protocol. *Most HFT-specific skill.* (Open-ended.)

---

## SECTION 13 — HOW THIS WILL BE EVALUATED (the metrics that matter)

The LLM should keep the student honest against these, because this is how interviewers and the student's own README will be judged.

### 13.1 Correctness (table stakes — without this nothing else counts)
- All unit tests + invariants pass.
- Determinism holds (same input → same output).
- Recovery restores exact state.

### 13.2 Performance (the quant metric)
- Reported with **percentiles** (p50/p99/p99.9), not averages.
- Backed by a **reproducible methodology** (machine, flags, data, warm-up).
- Shows a **measured improvement** from the optimization work (Milestone 1 vs Milestone 4) — the *delta* proves you did real optimization, not just got lucky.
- Bonus: the data-structure tradeoff benchmark (Milestone 3) — proves engineering judgment.

### 13.3 Systems robustness (the backend metric)
- Survives crash with zero data loss / zero corruption.
- Clean separation of the durable log from the in-memory book.
- (Stretch) Survives node failure via replication.

### 13.4 Engineering quality (judged by anyone reading the repo)
- Clean, commented, tested code.
- A README that leads with **numbers and a diagram**, explains the design and tradeoffs, and links the papers.
- A `docs/bench.md` with methodology.
- Honest scoping (clearly states what is and isn't implemented — never overclaims).

### 13.5 The interview test (the real evaluation)
Can the student, in 3 minutes, explain: what they built, why each data-structure choice, what the latency numbers are and how they measured them, what the hardest bug was, and how crash recovery works? If yes, the project has done its job. **A project you can defend deeply beats a flashier one you can't.**

---

## SECTION 14 — REQUIRED README STRUCTURE (the thing employers actually read)

The final GitHub README must contain, in this order:
1. **One-line description** + a one-sentence "what makes it interesting" (the dual quant/systems angle).
2. **Headline numbers**: throughput + p50/p99 latency, and the latency histogram image, right at the top.
3. **Architecture diagram** (the Section 5 diagram, cleaned up).
4. **Design decisions & tradeoffs**: the data-structure choice + the benchmark that justified it; the single-writer/ring-buffer rationale; the WAL/recovery design.
5. **How to build & run** (exact commands).
6. **Benchmark methodology** (link to `docs/bench.md`).
7. **What's implemented vs. not** (honest scope).
8. **Papers & references** this was built from (the Section 3 list).

A README like this makes a recruiter who skims for 20 seconds stop and read.

---

## SECTION 15 — COMMON PITFALLS (the LLM should watch for these)

- **Optimizing before it's correct.** Forbidden. Correct + tested first, then fast.
- **Allocating on the hot path.** Any `new`/`malloc`/`std::vector` growth inside the matching loop destroys latency. Pre-allocate everything.
- **Reporting averages.** Always percentiles. The tail is the story.
- **Fabricating numbers / copying a repo's benchmark.** The numbers must be the student's own, on their own machine. Fabrication is catastrophic if probed.
- **Letting the visualizer touch the hot path.** Observability must be isolated from the critical thread.
- **Trade executing at the wrong price.** Trades execute at the *resting* order's price. Easy to get wrong.
- **Losing time priority incorrectly.** A price modify loses priority (correct); a pure quantity-decrease should keep it (depends on exchange rules — pick one and document it).
- **Non-determinism leaking in.** No wall-clock or randomness in the matching path, or recovery breaks.
- **Overclaiming in the README / interview.** State exactly what's built. The student already values not bluffing — honor that.

---

## SECTION 16 — HOW TO TALK ABOUT IT (interview framing, both tracks)

**To a quant firm:** "I built a low-latency matching engine from the LMAX Disruptor paper and the CoinTossX architecture. It does price-time priority matching with limit/market/cancel, processes ~[X]M orders/sec single-threaded at p99 [Y]µs on replayed LOBSTER data. I benchmarked a Red-Black-tree vs flat-array price-level structure and [Z] won on my workload because [cache locality / memory]. Full methodology and histogram in the repo."

**To a systems/backend firm:** "I built a deterministic, single-writer matching engine backed by a write-ahead log — same shape as an event-sourced ledger. Every command is fsync'd before it's applied, so on crash it replays the log and recovers to an identical state; I demonstrate kill-and-recover with zero data loss. [Stretch: I put the command log under Raft so a backup takes over on primary failure.] It's essentially a tiny exchange, but the interesting part is the durability and determinism guarantees."

Same project. Two sentences. Two job offers' worth of signal.

---

*End of spec. Start at Section 12, Milestone 1. Build one milestone, test it, demo it, then continue. Don't read the rest again until you need it.*
