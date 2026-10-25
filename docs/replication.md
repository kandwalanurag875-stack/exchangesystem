# Milestone 7 — Raft replication (the fault-tolerant exchange)

The matching engine's command log is placed **under Raft consensus**, turning the
single-writer engine into a **replicated state machine**: a cluster of nodes each
applies the *same committed command sequence* and therefore holds an *identical*
order book. The leader replicates every order to a majority before committing;
kill the leader and a follower with the same log takes over — **zero data loss**.

This is the strongest systems statement in the project: it's the architecture of
etcd / Kafka / a replicated ledger, with the matching engine as the state machine.

## The core (`include/me/raft.h`)
A faithful implementation of the Raft algorithm (Ongaro & Ousterhout):

- **Leader election** — randomized election timeouts, terms, `RequestVote` with
  the *up-to-date log* restriction (a candidate with a shorter/older log can't
  win), majority vote.
- **Log replication** — `AppendEntries` with `prevLogIndex`/`prevLogTerm`
  consistency checks, conflict truncation, and heartbeats.
- **Commit safety** — the leader advances `commitIndex` only when a majority has
  the entry *and* it's from the current term (the paper's restriction that
  prevents committing stale entries by count alone). Committed entries are
  applied in order to the state machine.
- **Failover** — any higher term makes a node step down; a crashed leader is
  replaced by a follower that holds all committed entries.

### Designed for deterministic testing
The node is **transport-agnostic and clock-injected**: it never touches the
network or sleeps. It emits messages through a `send` callback and is advanced by
`tick(now_ms)`. That lets `tests/test_raft.cpp` wire N nodes in one process with
a **simulated network and a manual clock**, so election / replication / commit /
failover are tested with **zero flakiness**. Tests assert, among others, that 5
replicas converge to a **byte-identical book** and that after a leader crash the
committed entries survive on the new leader.

```bash
make tests_raft && ./build/tests_raft       # 5 deterministic Raft tests
```

## The live cluster (`apps/raft_node.cpp`)
A thin TCP driver around the same core. Threading is deliberate and is itself an
engineering point:

- **raft-loop thread** — the *only* thread touching the `RaftNode` + `OrderBook`.
  It drains inbound messages, ticks, proposes, and **enqueues** outbound
  messages. It performs **no blocking socket calls**, so a dead or slow peer can
  never stall consensus. *(An earlier inline-send version froze the whole loop
  the instant a peer died — the bug this design avoids.)*
- **one sender thread per peer** — owns that peer's socket, connects/reconnects,
  drains a bounded queue. Blocking only ever affects that one peer.
- **reader threads** — read length-framed messages into one inbound queue.

### Run the failover demo
```powershell
# Windows
.\build.ps1 raft_node
.\scripts\raft_failover_demo.ps1
```
```bash
# Linux/macOS — three terminals
P="0:127.0.0.1:9000,1:127.0.0.1:9001,2:127.0.0.1:9002"
./build/raft_node --id 0 --peers $P --rate 20
./build/raft_node --id 1 --peers $P --rate 20
./build/raft_node --id 2 --peers $P --rate 20
# ...then `kill -9` the leader process and watch a follower take over.
```

Observed run (the leader generates ~20 orders/s; status printed each second):
```
===== cluster running =====
[node 0] follower  term=3 leader=1 commit=55 applied=55 hash=016a41e0...
[node 1] LEADER    term=3 leader=1 commit=57 applied=57 hash=969277b5...
[node 2] follower  term=3 leader=1 commit=56 applied=56 hash=6f43e6b9...
*** KILLING leader node 1 ***
===== after leader crash =====
[node 0] LEADER    term=4 leader=0 commit=135 applied=135 hash=faf40f70bb5f33da
[node 2] follower  term=4 leader=0 commit=135 applied=135 hash=faf40f70bb5f33da
```
Node 0 won a new election at **term 4**, the cluster kept committing (55 → 135),
and the survivors share an **identical applied-state hash** — the replicated
books are the same down to the byte. That is the demoable guarantee: *kill the
primary, a backup takes over with no data loss.*

## Honest scope
- Implemented: leader election, log replication with conflict resolution,
  majority commit with the current-term safety rule, and failover.
- **Not** implemented: cluster membership changes (joint consensus) and Raft
  log snapshot/`InstallSnapshot`. The *state machine* already has its own
  snapshots (Milestone 5); static 3/5-node membership is enough to demonstrate
  the consensus guarantee. Raft persistent state (term/votedFor/log) is kept in
  memory in the demo — a production node would fsync it before responding, the
  same discipline as the Milestone 5 WAL.
- The demo's order ids are namespaced per node so a new leader never collides
  with ids already in the book.
