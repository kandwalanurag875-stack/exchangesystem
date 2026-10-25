# Milestone 8 — Real market-data feed handlers (ITCH 5.0 + LOBSTER)

The engine can reconstruct an order book from a **real exchange wire protocol**,
not just synthetic CSV. This is the most HFT-specific skill in the project: a
feed handler that turns an exchange's raw byte stream back into a live book.

> **Reconstruction, not matching.** A market-data feed already tells you exactly
> what happened. So feed events are *applied directly* to the book
> (add-resting / reduce / delete) — we do **not** re-run the matcher. The
> matching engine (M1) and the feed reconstructor share the same `OrderBook`
> data structure but drive it through different entry points (`apply()` for
> matching vs `loadResting`/`reduce`/`cancel` for reconstruction).

## NASDAQ TotalView-ITCH 5.0  (`include/me/itch.h`)

ITCH is the binary, big-endian, order-by-order feed NASDAQ broadcasts. We parse
the **BinaryFILE framing** (each message prefixed by a 2-byte big-endian length)
and handle the book-affecting message types:

| Type | Meaning | Book action |
|------|---------|-------------|
| `A` / `F` | Add order (no MPID / with MPID) | rest the order |
| `E` | Order executed | reduce that order by executed shares |
| `C` | Order executed with price | reduce that order |
| `X` | Order cancel (partial) | reduce that order |
| `D` | Order delete (full) | remove that order |
| `U` | Order replace | delete old ref, add new ref |
| `P` / `Q` | Trade / cross | counted only (no displayed-book change) |

Prices are 4-byte unsigned fixed-point with 4 implied decimals; we keep the raw
integer tick (the engine wants integer prices for determinism). Order reference
numbers (8 bytes) are used directly as the engine's `OrderId`.

### Run it
```bash
# generate a byte-accurate ITCH 5.0 file (no external download needed)
./build/gen_itch --out data/sample.itch --events 300000
# parse the real binary wire format and rebuild the book
./build/itch_replay data/sample.itch
```
On the dev box: **~4.2 M messages/sec** parse + reconstruct, and the rebuilt
resting-order count matches the generator exactly (`152371 == 152371`), with the
book invariants passing — evidence the byte layout and reconstruction are right.

> Point `itch_replay` at a real NASDAQ TotalView-ITCH 5.0 `*.bin` sample and it
> takes the identical code path. We generate our own file for tests so the suite
> never depends on a multi-GB download.

## LOBSTER  (`include/me/lobster.h`)

[LOBSTER](https://lobsterdata.com) publishes reconstructed NASDAQ order-book data
as two CSVs: a **message** file (events) and an **orderbook** file (top-N levels
after each event). We parse the message file:

| EventType | Meaning | Book action |
|-----------|---------|-------------|
| 1 | New limit order | rest the order (side from `Direction`) |
| 2 | Partial cancel | reduce |
| 3 | Full delete | remove |
| 4 | Execution of a visible limit order | reduce |
| 5 / 6 / 7 | Hidden execution / cross / halt | skipped (not in the visible book) |

### The reference cross-check on REAL data (Section 11 stretch)
Because LOBSTER ships the *answer* (the orderbook file), we validate the
reconstruction against it. A subtlety makes this interesting: **LOBSTER level
data omits the *opening* book** — the orders resting before the sample window
(9:30:00) aren't in the message file, so replaying messages from empty can't
rebuild the absolute book (the first orderbook row already shows a full book
after a single message). The rigorous method is therefore **per-step**: seed
from the *real* orderbook row *i−1*, apply message *i*, check it equals row *i*,
then re-seed from the real row *i*. Zero drift, and every one of the ~400k real
events is checked. This tests message *semantics*, which is the point.

```bash
# tiny bundled sample (message file only)
./build/lobster_replay data/lobster_sample_message.csv

# real LOBSTER sample with the per-step cross-check
./build/lobster_replay AAPL_..._message_10.csv \
    --orderbook AAPL_..._orderbook_10.csv --levels 10
```

**Result on real NASDAQ AAPL data (2012-06-21, 400,391 events):**

```
messages        : 400391  (191k add, 3k cancel, 171k delete, 24k execute, 11k hidden/cross/halt)
per-step validation vs real LOBSTER orderbook file:
  best bid/ask (L1) match : 100.0000%   (0 mismatches / 400,390 steps)
  top-5  match            : 100.0000%   (0 mismatches)
  top-10 (full) match     :  73.8850%   (depth-10 boundary: events that surface a hidden 11th level)
```

The top-10 figure is honest, not a bug: with only 10 levels of data, an event
that consumes a visible level surfaces an 11th level the snapshot never contained,
so the deepest level can't always be predicted from the previous 10. The best
bid/ask and top-5 — the part the data fully supports — match **exactly** on every
one of 400k real events.

**Across 4 real NASDAQ stocks (2012-06-21, level-10 samples):**

| stock | events | best bid/ask (L1) | top-5 |
|---|---:|---:|---:|
| AAPL | 400,391 | 100.0000% | 100.0000% |
| AMZN | 269,748 | 100.0000% | 100.0000% |
| GOOG | 147,916 | 100.0000% | 100.0000% |
| INTC | 624,040 | 100.0000% | 100.0000% |
| **total** | **1,442,095** | **100%** | **100%** |

Every stock, every event: the reconstruction's top-of-book matches LOBSTER's
reference exactly.

> The large LOBSTER CSVs are git-ignored (download them from the
> [HF mirror](https://huggingface.co/datasets/totalorganfailure/lobster-data) or
> [lobsterdata.com](https://lobsterdata.com)); only the tiny hand sample is committed.

## What's covered by tests (`tests/test_feed.cpp`)
- Big-endian field decode (prices/quantities rebuild at the right values).
- Execute / cancel / delete / replace reconstruct an exact, hand-computed book.
- A truncated tail is parsed safely (stops at the last whole message).
- `--limit`/`max_msgs` early stop.
- LOBSTER message parsing + reconstruction, and the cross-check helper.

## Honest scope
- ITCH: we handle the book-affecting and common admin messages; exotic NOII /
  auction-imbalance fields are skipped (counted), not interpreted.
- LOBSTER: hidden-order executions (type 5) don't touch the displayed book by
  definition, so they're skipped — the cross-check is against the *visible* book.
- The generator produces a clean, non-crossing stream; real venues are messier,
  but the parser/reconstructor logic is identical.
