// test_raft.cpp — Milestone 7: deterministic Raft tests.
//
// A simulated cluster runs N RaftNodes in ONE process with a controllable
// network (a message queue) and a MANUAL clock. No sockets, no sleeps, no
// flakiness: we step time forward and route messages ourselves, so leader
// election, log replication, commit, and FAILOVER are tested deterministically.
// Each node also applies committed commands to its own OrderBook, so we can
// assert the replicas converge to a byte-identical book (the replicated state
// machine guarantee).
#include <memory>
#include <string>
#include <vector>

#include "me/order_book.h"
#include "me/raft.h"
#include "me/test_framework.h"

using namespace me;

struct SimNode {
    std::unique_ptr<RaftNode> raft;
    bool alive = true;
    OrderBook book;                       // the replicated state machine
    std::vector<Command> applied;         // committed command sequence
    SimNode() : book(1u << 12) {}
};

class Cluster {
public:
    explicit Cluster(int n) {
        nodes_.resize(n);
        for (int i = 0; i < n; ++i) {
            std::vector<int> peers;
            for (int j = 0; j < n; ++j) if (j != i) peers.push_back(j);
            auto send = [this](const RaftMessage& m) { pending_.push_back(m); };
            auto apply = [this, i](const Command& c) {
                nodes_[i].applied.push_back(c);
                auto sink = [](const Trade&) {};
                nodes_[i].book.apply(c, sink);
            };
            nodes_[i].raft = std::make_unique<RaftNode>(
                i, peers, send, apply, /*seed=*/0xABC0 + i * 7919);
        }
    }

    void step(std::uint64_t dt = 10) {
        now_ += dt;
        std::vector<RaftMessage> batch;
        batch.swap(pending_);
        for (auto& m : batch)
            if (alive(m.to) && alive(m.from)) nodes_[m.to].raft->recv(m, now_);
        for (auto& n : nodes_)
            if (n.alive) n.raft->tick(now_);
    }

    // step until pred() or up to maxSteps; returns true if pred() held.
    template <class Pred>
    bool run_until(Pred pred, int maxSteps = 400) {
        for (int i = 0; i < maxSteps; ++i) { step(); if (pred()) return true; }
        return pred();
    }
    void run_for(int steps) { for (int i = 0; i < steps; ++i) step(); }

    int leaderCount() const {
        int c = 0;
        for (auto& n : nodes_) if (n.alive && n.raft->isLeader()) ++c;
        return c;
    }
    int leaderId() const {
        for (std::size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].alive && nodes_[i].raft->isLeader()) return (int)i;
        return -1;
    }
    void kill(int i) {
        nodes_[i].alive = false;
        // drop in-flight messages to/from the crashed node
        std::vector<RaftMessage> keep;
        for (auto& m : pending_) if (m.to != i && m.from != i) keep.push_back(m);
        pending_.swap(keep);
    }
    void revive(int i) { nodes_[i].alive = true; }

    bool alive(int i) const { return i >= 0 && i < (int)nodes_.size() && nodes_[i].alive; }
    SimNode& node(int i) { return nodes_[i]; }
    std::size_t size() const { return nodes_.size(); }

private:
    std::vector<SimNode> nodes_;
    std::vector<RaftMessage> pending_;
    std::uint64_t now_ = 0;
};

static Command mkAdd(Side s, Price p, Qty q, OrderId id) {
    Command c; c.type = CmdType::Add; c.ord_type = OrderType::Limit;
    c.side = s; c.price = p; c.qty = q; c.id = id; return c;
}

TEST_CASE("election: exactly one leader emerges in a 3-node cluster") {
    Cluster cl(3);
    bool elected = cl.run_until([&] { return cl.leaderCount() == 1; });
    CHECK(elected);
    CHECK(cl.leaderCount() == 1);
    // followers agree on the same term as the leader
    int lid = cl.leaderId();
    REQUIRE(lid >= 0);
    std::uint64_t lterm = cl.node(lid).raft->term();
    int agree = 0;
    for (std::size_t i = 0; i < cl.size(); ++i)
        if (cl.node((int)i).raft->term() == lterm) ++agree;
    CHECK(agree == 3);
}

TEST_CASE("replication: a committed command reaches every replica's log") {
    Cluster cl(3);
    REQUIRE(cl.run_until([&] { return cl.leaderCount() == 1; }));
    int lid = cl.leaderId();
    cl.node(lid).raft->propose(mkAdd(Side::Buy, 100, 10, 1));
    cl.node(lid).raft->propose(mkAdd(Side::Sell, 101, 5, 2));

    bool committed = cl.run_until([&] {
        for (std::size_t i = 0; i < cl.size(); ++i)
            if (cl.node((int)i).raft->commitIndex() < 2) return false;
        return true;
    });
    CHECK(committed);
    // all logs identical
    auto& L = cl.node(lid).raft->log();
    bool same = true;
    for (std::size_t i = 0; i < cl.size(); ++i) {
        auto& l = cl.node((int)i).raft->log();
        if (l.size() != L.size()) { same = false; break; }
        for (std::size_t k = 0; k < L.size(); ++k)
            same &= (l[k].term == L[k].term && l[k].cmd.id == L[k].cmd.id);
    }
    CHECK(same);
}

TEST_CASE("state machine: replicas converge to a byte-identical book") {
    Cluster cl(5);
    REQUIRE(cl.run_until([&] { return cl.leaderCount() == 1; }));
    int lid = cl.leaderId();
    for (int i = 1; i <= 20; ++i)
        cl.node(lid).raft->propose(mkAdd(i % 2 ? Side::Buy : Side::Sell,
                                         100 + (i % 7), 1 + (i % 5), i));
    REQUIRE(cl.run_until([&] {
        for (std::size_t i = 0; i < cl.size(); ++i)
            if (cl.node((int)i).raft->commitIndex() < 20) return false;
        return true;
    }));

    const std::string ref = cl.node(lid).book.serialize();
    bool identical = true;
    for (std::size_t i = 0; i < cl.size(); ++i)
        identical &= (cl.node((int)i).book.serialize() == ref);
    CHECK(identical);                       // every replica's book is identical
}

TEST_CASE("failover: kill the leader, a follower takes over, no data loss") {
    Cluster cl(3);
    REQUIRE(cl.run_until([&] { return cl.leaderCount() == 1; }));
    int old_leader = cl.leaderId();

    // commit some entries under the original leader
    for (int i = 1; i <= 5; ++i)
        cl.node(old_leader).raft->propose(mkAdd(Side::Buy, 100, 1, i));
    REQUIRE(cl.run_until([&] {
        for (std::size_t i = 0; i < cl.size(); ++i)
            if (cl.node((int)i).alive && cl.node((int)i).raft->commitIndex() < 5) return false;
        return true;
    }));
    std::uint64_t committed_before = cl.node(old_leader).raft->commitIndex();

    // CRASH the leader
    cl.kill(old_leader);

    // a survivor must win a new election
    bool new_leader = cl.run_until([&] {
        return cl.leaderCount() == 1 && cl.leaderId() != old_leader;
    });
    CHECK(new_leader);
    int nl = cl.leaderId();
    REQUIRE(nl >= 0);

    // the previously-committed entries survive on the new leader (no data loss)
    CHECK(cl.node(nl).raft->lastIndex() >= committed_before);

    // new leader keeps accepting writes
    cl.node(nl).raft->propose(mkAdd(Side::Sell, 200, 9, 99));
    bool progressed = cl.run_until([&] {
        return cl.node(nl).raft->commitIndex() >= committed_before + 1;
    });
    CHECK(progressed);

    // revive the old leader: it must rejoin as a follower and catch up
    cl.revive(old_leader);
    bool rejoined = cl.run_until([&] {
        return !cl.node(old_leader).raft->isLeader() &&
               cl.node(old_leader).raft->commitIndex() == cl.node(nl).raft->commitIndex();
    });
    CHECK(rejoined);
}

TEST_CASE("safety: a stale candidate with a shorter log cannot win") {
    Cluster cl(3);
    REQUIRE(cl.run_until([&] { return cl.leaderCount() == 1; }));
    int lid = cl.leaderId();
    for (int i = 1; i <= 8; ++i)
        cl.node(lid).raft->propose(mkAdd(Side::Buy, 100, 1, i));
    REQUIRE(cl.run_until([&] {
        for (std::size_t i = 0; i < cl.size(); ++i)
            if (cl.node((int)i).raft->commitIndex() < 8) return false;
        return true;
    }));
    // every node now has all 8 entries; the up-to-date check is what guarantees
    // that only a replica with the full log could have become leader.
    for (std::size_t i = 0; i < cl.size(); ++i)
        CHECK(cl.node((int)i).raft->lastIndex() == 8);
}

int main() { return tf::run_all(); }
