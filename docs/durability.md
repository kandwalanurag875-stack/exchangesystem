# Durability & Crash Recovery (Milestone 5)

> The systems story. Strip away the trading words and this is an **event-sourced,
> single-writer ledger**: every state change is a command appended to a durable
> log, and state is a deterministic fold over that log — the architecture of
> etcd, Kafka, and payment systems (Section 2). Reproduce:
> `./build/recover_demo selftest`.

## The contract

> **Every accepted command is on durable storage before it is applied, and after
> a crash the engine replays the log (from the latest snapshot) and rebuilds the
> EXACT same book** — because matching is deterministic (Section 4.5: no
> wall-clock, no randomness in the matching path).

## Pieces

### 1. Write-Ahead Log (`include/me/wal.h`)
- A compact **binary record per command**: magic, seq, type/side/ordtype/tif,
  id, price, qty, timestamp, and a **CRC32**. 52 bytes, `#pragma pack(1)`,
  append-only, sequential writes (fast — the spec's whole point about never
  random-accessing disk on the hot path).
- The timestamp is **audit-only** and never feeds matching, so determinism holds.
- **fsync discipline** (`durable_flush` = `fflush` + `fsync`/`_commit`): in
  `Durability::Strict` the record is forced to physical storage *before* the
  command is applied/acknowledged. This is the same WAL rule as a database (and
  the student's own DynamicKV on-disk layer).

### 2. Crash recovery (`MatchingEngine::recover`)
On startup: load the latest snapshot, then replay every WAL record after it,
re-applying each command. Determinism ⇒ the rebuilt book equals the pre-crash
book exactly.

### 3. Snapshots + log compaction (`write_snapshot` / `MatchingEngine::snapshot`)
A snapshot is the full resting book at a seq. Writing one **compacts the log**:
the snapshot is fsync'd and atomically renamed into place, *then* the WAL is
truncated — so a crash in between never loses committed state. Recovery then
replays only the short tail. (Same idea as Raft log compaction / LSM
checkpoints, Section 8.3.)

### 4. Torn-write & corruption tolerance
A crash mid-write leaves a partial trailing record. On replay, the **CRC fails**
(or the read is short) and replay **stops at the last fully valid record** — the
book is always consistent, never half-applied. A flipped bit anywhere truncates
replay at that record. Both are covered by tests.

## Proof it works

`recover_demo selftest --orders 200000` (one process: run → simulated crash →
recover):

```
BEFORE crash:  seq=200000 resting=19712 digest=4290954a25c10612
AFTER  recover: seq=200000 resting=19712 digest=4290954a25c10612 (replayed 100000)
RECOVERY PASS — recovered book is BYTE-IDENTICAL to pre-crash book.
```

Two-process demo (a real "kill it, restart it"):

```
$ recover_demo run --orders 50000      # writes WAL + a midway snapshot, then exits
RUN: seq=50000 resting=4959 book_digest=74a3fe09ec453166

$ recover_demo recover                 # a SEPARATE process
RECOVER: replayed 25000 WAL records
RECOVER: seq=50000 resting=4959 book_digest=74a3fe09ec453166
RECOVER: invariants=PASS
```

Same digest, fresh process, and only the 25,000-record tail replayed (the
snapshot covered the first half) — recovery **and** compaction, demonstrated.

## Test coverage (`tests/test_durability.cpp`)

| test | proves |
|---|---|
| WAL replay rebuilds identical book | basic recovery |
| snapshot + WAL tail | compaction path |
| determinism: recovered trades == fresh replay | Section 4.5 |
| torn write ignored | partial trailing record tolerated |
| flipped bit truncates replay | CRC catches corruption |

## Durability vs throughput (honest tradeoff)

`Durability::Strict` fsyncs **per command** — safest, but an fsync is ~tens of µs
to milliseconds depending on the device, so it caps throughput. `Durability::
Group` fsyncs every N commands (group commit): far higher throughput, at the
cost of losing the last `<N` un-fsync'd commands on a power cut. Real systems
pick group commit with a small window. Both are implemented; the mode is one
config field.
