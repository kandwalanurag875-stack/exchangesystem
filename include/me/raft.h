// raft.h — Milestone 7: the Raft consensus core (the strongest systems story).
//
// This is the algorithm from the Raft paper (Ongaro & Ousterhout, "In Search of
// an Understandable Consensus Algorithm"): leader election + log replication +
// commit on majority. The matching engine's command log is placed UNDER Raft,
// so the exchange becomes a *replicated state machine*: every replica applies
// the same committed command sequence and therefore holds an identical book.
// Kill the leader and a follower with the same log takes over — zero data loss.
//
// DESIGN FOR TESTABILITY (this is deliberate): the node is transport-agnostic
// and driven by an injected clock. It never calls the network or sleeps itself —
// it emits messages through a `send` callback and is advanced by tick(now_ms).
// That lets a test wire N nodes in ONE process with a simulated, controllable
// network and a manual clock, so election/replication/failover are tested
// DETERMINISTICALLY with no flaky timing. The TCP transport (raft_node.cpp) is a
// thin driver around the same core.
//
// Scope (honest): single-leader election, log replication with conflict
// resolution, majority commit, and failover. NOT implemented: cluster
// membership changes (joint consensus) and Raft log snapshotting/InstallSnapshot
// — the state machine already has its own snapshots (Milestone 5), and static
// 3/5-node membership is enough to demonstrate the guarantee.
#ifndef ME_RAFT_H
#define ME_RAFT_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "me/types.h"

namespace me {

enum class Role : std::uint8_t { Follower, Candidate, Leader };

struct LogEntry {
    std::uint64_t term = 0;
    Command       cmd{};
};

enum class RaftMsgType : std::uint8_t {
    RequestVote, RequestVoteResp, AppendEntries, AppendEntriesResp
};

// One struct carries every RPC (keeps the transport + test harness trivial).
struct RaftMessage {
    RaftMsgType   type;
    int           from = -1;
    int           to   = -1;
    std::uint64_t term = 0;

    // RequestVote
    std::uint64_t lastLogIndex = 0;
    std::uint64_t lastLogTerm  = 0;
    // RequestVoteResp
    bool          voteGranted = false;
    // AppendEntries
    int                   leaderId     = -1;
    std::uint64_t         prevLogIndex = 0;
    std::uint64_t         prevLogTerm  = 0;
    std::uint64_t         leaderCommit = 0;
    std::vector<LogEntry> entries;
    // AppendEntriesResp
    bool          success    = false;
    std::uint64_t matchIndex = 0;   // follower's last matching index (nextIndex hint)
};

class RaftNode {
public:
    using SendFn  = std::function<void(const RaftMessage&)>;
    using ApplyFn = std::function<void(const Command&)>;

    // Timing (ms). Election timeout is randomized in [min,max] per the paper to
    // avoid split votes; heartbeat must be << election timeout.
    std::uint64_t election_min_ms = 150;
    std::uint64_t election_max_ms = 300;
    std::uint64_t heartbeat_ms    = 50;

    RaftNode(int id, std::vector<int> peers, SendFn send, ApplyFn apply,
             std::uint64_t seed = 0)
        : id_(id), peers_(std::move(peers)), send_(std::move(send)),
          apply_(std::move(apply)),
          rng_(seed ? seed : 0x9E3779B97F4A7C15ull ^ (std::uint64_t(id) * 2654435761u)) {
        for (int p : peers_) { next_index_[p] = 1; match_index_[p] = 0; }
    }

    // ---- driving the node ------------------------------------------------
    void tick(std::uint64_t now_ms) {
        if (role_ == Role::Leader) {
            if (now_ms >= heartbeat_deadline_) {
                for (int p : peers_) sendAppend(p);
                heartbeat_deadline_ = now_ms + heartbeat_ms;
            }
        } else if (now_ms >= election_deadline_) {
            startElection(now_ms);
        }
    }

    void recv(const RaftMessage& m, std::uint64_t now_ms) {
        // Any higher term => step down and adopt it (Raft rule 1).
        if (m.term > term_) becomeFollower(m.term, /*clearVote=*/true, now_ms);
        switch (m.type) {
            case RaftMsgType::RequestVote:       onRequestVote(m, now_ms); break;
            case RaftMsgType::RequestVoteResp:   onRequestVoteResp(m, now_ms); break;
            case RaftMsgType::AppendEntries:     onAppendEntries(m, now_ms); break;
            case RaftMsgType::AppendEntriesResp: onAppendEntriesResp(m, now_ms); break;
        }
    }

    // Client request: only the leader accepts. Appends to the local log and
    // kicks replication. Returns the assigned index (0 if not leader).
    std::uint64_t propose(const Command& c) {
        if (role_ != Role::Leader) return 0;
        log_.push_back(LogEntry{term_, c});
        match_index_[id_] = lastIndex();        // leader matches its own log
        for (int p : peers_) sendAppend(p);
        return lastIndex();
    }

    // ---- observation -----------------------------------------------------
    Role          role() const { return role_; }
    std::uint64_t term() const { return term_; }
    int           id() const { return id_; }
    int           leaderId() const { return leader_id_; }
    bool          isLeader() const { return role_ == Role::Leader; }
    std::uint64_t commitIndex() const { return commit_index_; }
    std::uint64_t lastApplied() const { return last_applied_; }
    std::uint64_t lastIndex() const { return log_.size(); }
    const std::vector<LogEntry>& log() const { return log_; }
    int           votedFor() const { return voted_for_; }

private:
    std::size_t majority() const { return (peers_.size() + 1) / 2 + 1; }
    std::uint64_t entryTerm(std::uint64_t index) const {
        return (index == 0 || index > log_.size()) ? 0 : log_[index - 1].term;
    }

    void resetElectionDeadline(std::uint64_t now_ms) {
        std::uniform_int_distribution<std::uint64_t> d(election_min_ms, election_max_ms);
        election_deadline_ = now_ms + d(rng_);
    }

    void becomeFollower(std::uint64_t term, bool clearVote, std::uint64_t now_ms) {
        role_ = Role::Follower;
        term_ = term;
        if (clearVote) voted_for_ = -1;
        resetElectionDeadline(now_ms);
    }

    void startElection(std::uint64_t now_ms) {
        role_ = Role::Candidate;
        ++term_;
        voted_for_ = id_;
        votes_.clear();
        votes_.insert(id_);
        resetElectionDeadline(now_ms);
        for (int p : peers_) {
            RaftMessage m;
            m.type = RaftMsgType::RequestVote;
            m.from = id_; m.to = p; m.term = term_;
            m.lastLogIndex = lastIndex();
            m.lastLogTerm  = entryTerm(lastIndex());
            send_(m);
        }
        // single-node cluster: we are already a majority
        if (votes_.size() >= majority()) becomeLeader(now_ms);
    }

    void becomeLeader(std::uint64_t now_ms) {
        role_ = Role::Leader;
        leader_id_ = id_;
        for (int p : peers_) { next_index_[p] = lastIndex() + 1; match_index_[p] = 0; }
        match_index_[id_] = lastIndex();
        heartbeat_deadline_ = now_ms;            // beat immediately
        for (int p : peers_) sendAppend(p);
    }

    bool candidateUpToDate(std::uint64_t lastLogIndex, std::uint64_t lastLogTerm) const {
        const std::uint64_t myTerm = entryTerm(lastIndex());
        if (lastLogTerm != myTerm) return lastLogTerm > myTerm;
        return lastLogIndex >= lastIndex();
    }

    void onRequestVote(const RaftMessage& m, std::uint64_t now_ms) {
        RaftMessage r;
        r.type = RaftMsgType::RequestVoteResp;
        r.from = id_; r.to = m.from; r.term = term_; r.voteGranted = false;
        if (m.term >= term_ &&
            (voted_for_ == -1 || voted_for_ == m.from) &&
            candidateUpToDate(m.lastLogIndex, m.lastLogTerm)) {
            voted_for_ = m.from;
            r.voteGranted = true;
            resetElectionDeadline(now_ms);       // we acknowledged a viable leader
        }
        send_(r);
    }

    void onRequestVoteResp(const RaftMessage& m, std::uint64_t now_ms) {
        if (role_ != Role::Candidate || m.term != term_) return;
        if (m.voteGranted) {
            votes_.insert(m.from);
            if (votes_.size() >= majority()) becomeLeader(now_ms);
        }
    }

    void sendAppend(int peer) {
        const std::uint64_t ni = next_index_[peer];
        const std::uint64_t prev = ni - 1;
        RaftMessage m;
        m.type = RaftMsgType::AppendEntries;
        m.from = id_; m.to = peer; m.term = term_;
        m.leaderId = id_;
        m.prevLogIndex = prev;
        m.prevLogTerm  = entryTerm(prev);
        m.leaderCommit = commit_index_;
        for (std::uint64_t i = ni; i <= lastIndex(); ++i) m.entries.push_back(log_[i - 1]);
        send_(m);
    }

    void onAppendEntries(const RaftMessage& m, std::uint64_t now_ms) {
        RaftMessage r;
        r.type = RaftMsgType::AppendEntriesResp;
        r.from = id_; r.to = m.from; r.term = term_;
        r.success = false; r.matchIndex = 0;

        if (m.term < term_) { send_(r); return; }       // stale leader

        // Valid current-term leader: defer to it.
        role_ = Role::Follower;
        leader_id_ = m.leaderId;
        resetElectionDeadline(now_ms);

        // Log-consistency check: our entry at prevLogIndex must match.
        if (m.prevLogIndex > lastIndex() ||
            entryTerm(m.prevLogIndex) != m.prevLogTerm) {
            send_(r);                                    // success=false -> leader backs up
            return;
        }

        // Append, truncating on the first conflicting term.
        std::uint64_t idx = m.prevLogIndex;
        for (const auto& e : m.entries) {
            ++idx;
            if (idx <= lastIndex()) {
                if (entryTerm(idx) != e.term) {
                    log_.resize(idx - 1);                // drop conflict + everything after
                    log_.push_back(e);
                }
                // else already present and matching -> skip
            } else {
                log_.push_back(e);
            }
        }

        if (m.leaderCommit > commit_index_) {
            commit_index_ = std::min(m.leaderCommit, lastIndex());
            applyCommitted();
        }
        r.success = true;
        r.matchIndex = idx;                              // last index we now agree on
        send_(r);
    }

    void onAppendEntriesResp(const RaftMessage& m, std::uint64_t /*now*/) {
        if (role_ != Role::Leader || m.term != term_) return;
        if (m.success) {
            match_index_[m.from] = std::max(match_index_[m.from], m.matchIndex);
            next_index_[m.from]  = m.matchIndex + 1;
            advanceCommit();
        } else {
            // back up nextIndex and retry (linear backoff is enough here)
            if (next_index_[m.from] > 1) --next_index_[m.from];
            sendAppend(m.from);
        }
    }

    // Leader commit rule: advance commitIndex to the highest N where a MAJORITY
    // has matchIndex >= N AND log[N] is from the current term (the paper's
    // safety restriction that prevents committing stale entries by count alone).
    void advanceCommit() {
        for (std::uint64_t N = lastIndex(); N > commit_index_; --N) {
            if (entryTerm(N) != term_) continue;
            std::size_t count = 0;
            if (match_index_[id_] >= N) ++count;         // self
            for (int p : peers_) if (match_index_[p] >= N) ++count;
            if (count >= majority()) { commit_index_ = N; applyCommitted(); break; }
        }
    }

    void applyCommitted() {
        while (last_applied_ < commit_index_) {
            ++last_applied_;
            apply_(log_[last_applied_ - 1].cmd);
        }
    }

    // identity / config
    int id_;
    std::vector<int> peers_;
    SendFn  send_;
    ApplyFn apply_;
    std::mt19937_64 rng_;

    // persistent state (would be fsync'd before responding in a full impl)
    std::uint64_t term_ = 0;
    int           voted_for_ = -1;
    std::vector<LogEntry> log_;

    // volatile state
    Role role_ = Role::Follower;
    int  leader_id_ = -1;
    std::uint64_t commit_index_ = 0;
    std::uint64_t last_applied_ = 0;
    std::uint64_t election_deadline_ = 0;
    std::uint64_t heartbeat_deadline_ = 0;
    std::unordered_set<int> votes_;

    // leader state
    std::unordered_map<int, std::uint64_t> next_index_;
    std::unordered_map<int, std::uint64_t> match_index_;
};

}  // namespace me

#endif  // ME_RAFT_H
