// raft_node.cpp — Milestone 7: one node of a Raft-replicated matching engine,
// over real TCP. Run 3 (or 5) of these and you have a fault-tolerant exchange:
// the leader replicates every order to a majority before committing, each node
// applies the same committed command stream to its own book, and killing the
// leader makes a follower take over with an identical log — zero data loss.
//
//   raft_node --id 0 --peers 0:127.0.0.1:9000,1:127.0.0.1:9001,2:127.0.0.1:9002
//             [--rate 20]        # orders/sec the leader generates (demo load)
//
// THREADING (and why it's correct):
//   * raft-loop thread  — the ONLY thread touching the RaftNode + OrderBook. It
//     drains inbound messages, ticks, proposes, and ENQUEUES outbound messages.
//     It never calls a blocking socket op, so a dead/slow peer can never stall
//     consensus (the bug a naive inline-send version has).
//   * one sender thread per peer — owns that peer's socket, connects/reconnects,
//     and drains an outbound queue. Blocking here only ever affects that peer.
//   * reader threads — read framed messages off inbound sockets into one queue.
#include "me/net.h"            // MUST precede <windows.h>-pulling headers

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "me/order_book.h"
#include "me/raft.h"

using namespace me;

// ----------------------------- wire encoding ---------------------------------
static void putU8(std::vector<std::uint8_t>& b, std::uint8_t v) { b.push_back(v); }
static void putU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
static void putU64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((v >> (8 * i)) & 0xFF);
}
struct Reader { const std::uint8_t* p; std::size_t n, i = 0;
    std::uint8_t  u8()  { return i < n ? p[i++] : 0; }
    std::uint32_t u32() { std::uint32_t v = 0; for (int k = 0; k < 4; ++k) v |= std::uint32_t(u8()) << (8 * k); return v; }
    std::uint64_t u64() { std::uint64_t v = 0; for (int k = 0; k < 8; ++k) v |= std::uint64_t(u8()) << (8 * k); return v; }
};

static void encodeCmd(std::vector<std::uint8_t>& b, const Command& c) {
    putU8(b, (std::uint8_t)c.type); putU8(b, (std::uint8_t)c.side);
    putU8(b, (std::uint8_t)c.ord_type); putU8(b, (std::uint8_t)c.tif);
    putU64(b, c.id); putU64(b, (std::uint64_t)c.price); putU64(b, c.qty);
}
static Command decodeCmd(Reader& r) {
    Command c;
    c.type = (CmdType)r.u8(); c.side = (Side)r.u8();
    c.ord_type = (OrderType)r.u8(); c.tif = (Tif)r.u8();
    c.id = r.u64(); c.price = (Price)r.u64(); c.qty = r.u64();
    return c;
}
static std::vector<std::uint8_t> encode(const RaftMessage& m) {
    std::vector<std::uint8_t> b;
    putU8(b, (std::uint8_t)m.type);
    putU32(b, (std::uint32_t)m.from); putU32(b, (std::uint32_t)m.to);
    putU64(b, m.term);
    putU64(b, m.lastLogIndex); putU64(b, m.lastLogTerm);
    putU8(b, m.voteGranted ? 1 : 0);
    putU32(b, (std::uint32_t)m.leaderId);
    putU64(b, m.prevLogIndex); putU64(b, m.prevLogTerm); putU64(b, m.leaderCommit);
    putU8(b, m.success ? 1 : 0);
    putU64(b, m.matchIndex);
    putU32(b, (std::uint32_t)m.entries.size());
    for (auto& e : m.entries) { putU64(b, e.term); encodeCmd(b, e.cmd); }
    return b;
}
static RaftMessage decode(const std::vector<std::uint8_t>& bytes) {
    Reader r{bytes.data(), bytes.size()};
    RaftMessage m;
    m.type = (RaftMsgType)r.u8();
    m.from = (int)r.u32(); m.to = (int)r.u32();
    m.term = r.u64();
    m.lastLogIndex = r.u64(); m.lastLogTerm = r.u64();
    m.voteGranted = r.u8() != 0;
    m.leaderId = (int)r.u32();
    m.prevLogIndex = r.u64(); m.prevLogTerm = r.u64(); m.leaderCommit = r.u64();
    m.success = r.u8() != 0;
    m.matchIndex = r.u64();
    std::uint32_t ne = r.u32();
    for (std::uint32_t k = 0; k < ne; ++k) { LogEntry e; e.term = r.u64(); e.cmd = decodeCmd(r); m.entries.push_back(e); }
    return m;
}

// ----------------------------- inbound (readers) -----------------------------
static std::vector<RaftMessage> g_inbound;
static std::mutex               g_inbox_mtx;
static std::atomic<bool>        g_running{true};

static std::uint64_t now_ms() {
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void reader_thread(socket_t c) {
    std::vector<std::uint8_t> buf;
    while (g_running.load()) {
        if (!net_recv_framed(c, buf)) break;
        RaftMessage m = decode(buf);
        std::lock_guard<std::mutex> lk(g_inbox_mtx);
        g_inbound.push_back(std::move(m));
    }
    net_close(c);
}
static void listener_thread(socket_t srv) {
    while (g_running.load()) {
        socket_t c = ::accept(srv, nullptr, nullptr);
        if (!net_valid(c)) { if (!g_running.load()) break; continue; }
        std::thread(reader_thread, c).detach();
    }
}

// ----------------------------- outbound (per-peer sender) --------------------
// One thread per peer owns the socket and drains a bounded queue. The raft loop
// only enqueues, so it NEVER blocks on the network — the fix for the freeze a
// naive inline net_connect()/net_send() in the loop suffers when a peer dies.
struct Peer {
    int id; std::string host; int port;
    std::mutex m; std::condition_variable cv;
    std::deque<std::vector<std::uint8_t>> q;
    std::thread th;
};
static void sender_loop(Peer* p) {
    socket_t out = INVALID_SOCKET; bool conn = false;
    while (g_running.load()) {
        std::vector<std::uint8_t> msg;
        {
            std::unique_lock<std::mutex> lk(p->m);
            p->cv.wait_for(lk, std::chrono::milliseconds(200),
                           [&] { return !g_running.load() || !p->q.empty(); });
            if (!g_running.load() && p->q.empty()) break;
            if (p->q.empty()) continue;
            msg = std::move(p->q.front()); p->q.pop_front();
        }
        if (!conn) { out = net_connect(p->host, p->port); conn = net_valid(out); }
        if (conn && !net_send_framed(out, msg)) { net_close(out); conn = false; }
        // if still not connected, the message is dropped — Raft tolerates loss.
    }
    if (conn) net_close(out);
}
static void peer_enqueue(Peer* p, std::vector<std::uint8_t>&& bytes) {
    {
        std::lock_guard<std::mutex> lk(p->m);
        if (p->q.size() >= 1024) p->q.pop_front();     // bound memory on a dead peer
        p->q.push_back(std::move(bytes));
    }
    p->cv.notify_one();
}

int main(int argc, char** argv) {
    int self_id = 0, rate = 20;
    std::string peers_arg;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--id") && i + 1 < argc) self_id = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--peers") && i + 1 < argc) peers_arg = argv[++i];
        else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atoi(argv[++i]);
    }
    if (peers_arg.empty()) {
        std::printf("usage: raft_node --id N --peers 0:host:port,1:host:port,... [--rate R]\n");
        return 1;
    }

    std::unordered_map<int, std::pair<std::string, int>> addr;
    std::vector<int> all_ids;
    { std::stringstream ss(peers_arg); std::string tok;
      while (std::getline(ss, tok, ',')) {
          auto c1 = tok.find(':'), c2 = tok.rfind(':');
          if (c1 == std::string::npos || c1 == c2) continue;
          int id = std::atoi(tok.substr(0, c1).c_str());
          std::string host = tok.substr(c1 + 1, c2 - c1 - 1);
          int port = std::atoi(tok.substr(c2 + 1).c_str());
          addr[id] = {host, port}; all_ids.push_back(id);
      } }
    if (!addr.count(self_id)) { std::printf("self id %d not in --peers\n", self_id); return 1; }
    const int self_port = addr[self_id].second;

    std::vector<int> peer_ids;
    std::vector<std::unique_ptr<Peer>> peers;
    std::unordered_map<int, Peer*> peer_by_id;
    for (int id : all_ids) if (id != self_id) {
        peer_ids.push_back(id);
        auto p = std::make_unique<Peer>();
        p->id = id; p->host = addr[id].first; p->port = addr[id].second;
        peer_by_id[id] = p.get();
        peers.push_back(std::move(p));
    }
    for (auto& p : peers) p->th = std::thread(sender_loop, p.get());

    // replicated state machine: the order book
    OrderBook book(1u << 16);
    std::uint64_t applied = 0, app_hash = 1469598103934665603ull;   // FNV-1a
    auto fnv = [&](std::uint64_t x) { app_hash ^= x; app_hash *= 1099511628211ull; };

    auto send_cb = [&](const RaftMessage& m) {
        auto it = peer_by_id.find(m.to);
        if (it != peer_by_id.end()) peer_enqueue(it->second, encode(m));
    };
    auto apply_cb = [&](const Command& c) {
        auto sink = [](const Trade&) {};
        book.apply(c, sink);
        ++applied; fnv(c.id); fnv((std::uint64_t)c.price); fnv(c.qty);
    };

    RaftNode raft(self_id, peer_ids, send_cb, apply_cb, /*seed=*/0xC0FFEEu + self_id);

    socket_t srv = net_listen(self_port);
    if (!net_valid(srv)) { std::printf("listen(%d) failed\n", self_port); return 1; }
    std::thread(listener_thread, srv).detach();
    std::printf("[node %d] listening on 127.0.0.1:%d, peers:", self_id, self_port);
    for (auto& p : peers) std::printf(" %d@%s:%d", p->id, p->host.c_str(), p->port);
    std::printf("\n"); std::fflush(stdout);

    std::uint64_t gen_ctr = 0;
    auto genOrder = [&]() {
        Command c; c.type = CmdType::Add; c.ord_type = OrderType::Limit;
        ++gen_ctr;
        c.side = (gen_ctr & 1) ? Side::Buy : Side::Sell;
        const Price mid = 10000;
        c.price = (c.side == Side::Buy) ? mid - 1 - (Price)(gen_ctr % 5)
                                        : mid + 1 + (Price)(gen_ctr % 5);
        c.qty = 1 + (gen_ctr % 10);
        c.id = ((std::uint64_t)(self_id + 1) << 48) | gen_ctr;   // node-namespaced ids
        return c;
    };

    std::uint64_t next_status = now_ms() + 1000;
    std::uint64_t next_order  = now_ms() + 500;
    const std::uint64_t order_gap = rate > 0 ? (1000 / (std::uint64_t)rate) : 1000;

    while (g_running.load()) {
        const std::uint64_t t = now_ms();
        std::vector<RaftMessage> drained;
        { std::lock_guard<std::mutex> lk(g_inbox_mtx); drained.swap(g_inbound); }
        for (auto& m : drained) raft.recv(m, t);
        raft.tick(t);
        if (raft.isLeader() && t >= next_order) { raft.propose(genOrder()); next_order = t + order_gap; }

        if (t >= next_status) {
            const char* role = raft.isLeader() ? "LEADER  " :
                               (raft.role() == Role::Candidate ? "CANDIDATE" : "follower");
            Price bb = 0, ba = 0; book.bestBid(bb); book.bestAsk(ba);
            std::printf("[node %d] %s term=%llu leader=%d commit=%llu applied=%llu "
                        "resting=%zu bid=%lld ask=%lld hash=%016llx\n",
                        self_id, role, (unsigned long long)raft.term(), raft.leaderId(),
                        (unsigned long long)raft.commitIndex(), (unsigned long long)applied,
                        book.restingCount(), (long long)bb, (long long)ba,
                        (unsigned long long)app_hash);
            std::fflush(stdout);
            next_status = t + 1000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    g_running.store(false);
    for (auto& p : peers) { p->cv.notify_all(); if (p->th.joinable()) p->th.join(); }
    return 0;
}
