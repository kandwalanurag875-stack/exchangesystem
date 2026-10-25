// feed_server.cpp — Milestone 6: the live order-book visualizer's backend.
//
// HONESTY CAVEAT (Section 10.3): this is a PRESENTATION aid, not part of a real
// HFT system — real venues are headless. It exists to make the engine visible
// and to reuse web skills. In an interview it's "a visualization layer I added,
// deliberately OFF the latency-critical path."
//
// Architecture (Section 10.1 / 10.4 — the part that IS an engineering point):
//   * Thread M (matching/writer): the ONLY mutator of the book. It runs the
//     engine flat-out and, at a fixed cadence (~100ms), copies a small top-N
//     snapshot into a mutex-guarded buffer. Snapshotting is the matching
//     thread's own cheap read — it never blocks on the network.
//   * Thread(s) N (network): a from-scratch HTTP server pushing the snapshot to
//     browsers via Server-Sent Events. If a client is slow or disconnects, the
//     matching thread is UNAFFECTED — observability is fully isolated from the
//     critical path. That isolation is the real thing to be able to explain.
//
//   feed_server [--port 8080] [--rate 30000]
// Platform sockets FIRST: on Windows winsock2.h must precede any windows.h, and
// NOMINMAX stops windows.h's min/max macros from clobbering std::min/std::max
// used inside the order book.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static int sock_close(socket_t s) { return closesocket(s); }
static void net_init() { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static const socket_t INVALID_SOCKET = -1;
static int sock_close(socket_t s) { return ::close(s); }
static void net_init() {}
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "me/order_book.h"
#include "me/types.h"

using namespace me;

// ---- shared snapshot (the only thing crossing the M/N thread boundary) ------
struct Snapshot {
    std::vector<std::pair<Price, Qty>> bids, asks;   // top-N, best first
    Price last_price = 0; Qty last_qty = 0;
    double tps = 0;                                   // trades per second
    long long spread = 0;
    std::uint64_t total_trades = 0, processed = 0;
};
static std::mutex g_mtx;
static Snapshot g_snap;
static std::atomic<bool> g_running{true};

// ---- a small live order generator (mean-reverting, bounded book) ------------
struct LiveGen {
    std::mt19937_64 rng{0xABCDEF};
    std::vector<OrderId> live;
    OrderId next_id = 1;
    Price mid = 10000;

    Command next() {
        std::uniform_real_distribution<double> u(0, 1);
        std::uniform_int_distribution<std::uint64_t> qd(1, 50);
        const double r = u(rng);
        Command c{};
        // Keep the book from growing without bound: cancel more when it's big.
        const double p_cancel = live.size() > 4000 ? 0.45 : 0.20;
        if (r < p_cancel && !live.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            std::size_t i = pick(rng);
            c.type = CmdType::Cancel; c.id = live[i];
            live[i] = live.back(); live.pop_back();
        } else if (r < p_cancel + 0.10) {
            c.type = CmdType::Add; c.ord_type = OrderType::Market;
            c.side = u(rng) < 0.5 ? Side::Buy : Side::Sell; c.qty = qd(rng);
            c.id = next_id++;
        } else {
            std::normal_distribution<double> pd(double(mid), 8.0);
            c.type = CmdType::Add; c.ord_type = OrderType::Limit;
            c.side = u(rng) < 0.5 ? Side::Buy : Side::Sell;
            c.price = (Price)pd(rng); c.qty = qd(rng);
            c.id = next_id; live.push_back(next_id); ++next_id;
        }
        return c;
    }
};

// ---- thread M: run the engine, publish snapshots ----------------------------
static void matching_thread(unsigned rate) {
    OrderBook book(1u << 20);
    LiveGen gen;
    Price last_price = 0; Qty last_qty = 0;
    std::uint64_t total_trades = 0, processed = 0;
    auto sink = [&](const Trade& t) { last_price = t.price; last_qty = t.qty; ++total_trades; };

    auto last_pub = std::chrono::steady_clock::now();
    auto rate_t0 = last_pub;
    std::uint64_t trades_at_t0 = 0;
    const auto batch_sleep = std::chrono::microseconds(rate ? (1000000ull * 200 / rate) : 0);

    std::vector<std::pair<Price, Qty>> bids, asks;
    while (g_running.load()) {
        for (int i = 0; i < 200; ++i) { book.apply(gen.next(), sink); ++processed; }
        if (batch_sleep.count() > 0) std::this_thread::sleep_for(batch_sleep);

        const auto now = std::chrono::steady_clock::now();
        if (now - last_pub >= std::chrono::milliseconds(100)) {
            book.topBids(15, bids);
            book.topAsks(15, asks);
            Price bb = 0, ba = 0;
            const bool hb = book.bestBid(bb), ha = book.bestAsk(ba);
            const double secs = std::chrono::duration<double>(now - rate_t0).count();
            const double tps = secs > 0 ? (total_trades - trades_at_t0) / secs : 0;
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_snap.bids = bids; g_snap.asks = asks;
                g_snap.last_price = last_price; g_snap.last_qty = last_qty;
                g_snap.spread = (hb && ha) ? (long long)(ba - bb) : 0;
                g_snap.tps = tps;
                g_snap.total_trades = total_trades;
                g_snap.processed = processed;
            }
            last_pub = now;
            if (now - rate_t0 >= std::chrono::seconds(1)) { rate_t0 = now; trades_at_t0 = total_trades; }
        }
    }
}

// ---- JSON (hand-rolled; no dependency) --------------------------------------
static std::string snapshot_json() {
    Snapshot s;
    { std::lock_guard<std::mutex> lk(g_mtx); s = g_snap; }
    std::string j = "{\"bids\":[";
    for (std::size_t i = 0; i < s.bids.size(); ++i)
        j += (i ? "," : "") + ("[" + std::to_string(s.bids[i].first) + "," +
              std::to_string(s.bids[i].second) + "]");
    j += "],\"asks\":[";
    for (std::size_t i = 0; i < s.asks.size(); ++i)
        j += (i ? "," : "") + ("[" + std::to_string(s.asks[i].first) + "," +
              std::to_string(s.asks[i].second) + "]");
    j += "],\"last\":{\"price\":" + std::to_string(s.last_price) +
         ",\"qty\":" + std::to_string(s.last_qty) + "}";
    j += ",\"spread\":" + std::to_string(s.spread);
    j += ",\"tps\":" + std::to_string((long long)s.tps);
    j += ",\"trades\":" + std::to_string(s.total_trades);
    j += ",\"processed\":" + std::to_string(s.processed) + "}";
    return j;
}

static bool send_all(socket_t c, const char* p, std::size_t n) {
    while (n) {
        int sent = ::send(c, p, (int)n, 0);
        if (sent <= 0) return false;
        p += sent; n -= (std::size_t)sent;
    }
    return true;
}

// ---- the embedded standalone page (so the demo needs only this binary) ------
static const char* kPage = R"HTML(<!doctype html><html><head><meta charset=utf8>
<title>Mini-Exchange — live order book</title><style>
body{background:#0b0e14;color:#cdd6f4;font:14px/1.4 ui-monospace,Menlo,Consolas,monospace;margin:0;padding:24px}
h1{font-size:16px;color:#89b4fa;margin:0 0 4px} .sub{color:#6c7086;margin-bottom:18px}
.book{max-width:560px} .row{display:flex;align-items:center;height:22px;position:relative}
.row .bar{position:absolute;right:50%;height:18px;opacity:.35;border-radius:2px}
.row.ask .bar{right:auto;left:50%;background:#f38ba8} .row.bid .bar{background:#a6e3a1}
.px{position:absolute;left:50%;transform:translateX(-50%);width:90px;text-align:center;z-index:2}
.qty{position:absolute;z-index:2;color:#9399b2} .row.ask .qty{left:8px} .row.bid .qty{right:8px}
.mid{height:30px;display:flex;align-items:center;justify-content:center;color:#f9e2af;border-top:1px solid #313244;border-bottom:1px solid #313244;margin:2px 0}
.tick{margin-top:18px;display:flex;gap:24px;color:#cdd6f4} .tick b{color:#89b4fa}
.up{color:#a6e3a1}.down{color:#f38ba8}</style></head><body>
<h1>Mini-Exchange — live order book</h1>
<div class=sub>single-writer matching engine · snapshots pushed off the hot path via SSE</div>
<div class=book id=book></div>
<div class=tick>
 <div>last <b id=last>–</b></div><div>spread <b id=spread>–</b></div>
 <div>trades/s <b id=tps>–</b></div><div>trades <b id=trades>–</b></div>
 <div>processed <b id=proc>–</b></div></div>
<script>
const book=document.getElementById('book');let prev=null;
function bar(side,px,qty,max){const pct=Math.max(2,100*qty/max);
 return `<div class="row ${side}"><div class="bar" style="width:${pct*0.48}%"></div>`+
        `<div class=px>${px}</div><div class=qty>${qty}</div></div>`;}
const ev=new EventSource('/events');
ev.onmessage=e=>{const s=JSON.parse(e.data);
 const all=[...s.bids.map(x=>x[1]),...s.asks.map(x=>x[1]),1];const max=Math.max(...all);
 let h='';for(const a of [...s.asks].reverse())h+=bar('ask',a[0],a[1],max);
 h+=`<div class=mid>— spread ${s.spread} —</div>`;
 for(const b of s.bids)h+=bar('bid',b[0],b[1],max);book.innerHTML=h;
 const l=document.getElementById('last');const dir=prev!=null?(s.last.price>prev?'up':s.last.price<prev?'down':''):'';
 l.className=dir;l.textContent=`${s.last.qty} @ ${s.last.price}`;prev=s.last.price;
 document.getElementById('spread').textContent=s.spread;
 document.getElementById('tps').textContent=s.tps;
 document.getElementById('trades').textContent=s.trades;
 document.getElementById('proc').textContent=s.processed;};
</script></body></html>)HTML";

// ---- a client connection (its own thread) -----------------------------------
static void handle_client(socket_t c) {
    char buf[2048];
    int n = ::recv(c, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { sock_close(c); return; }
    buf[n] = 0;
    // crude routing on the request path
    std::string req(buf);
    std::string path = "/";
    {
        auto sp = req.find(' ');
        if (sp != std::string::npos) {
            auto sp2 = req.find(' ', sp + 1);
            if (sp2 != std::string::npos) path = req.substr(sp + 1, sp2 - sp - 1);
        }
    }

    if (path.rfind("/events", 0) == 0) {
        const char* hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n";
        if (!send_all(c, hdr, std::strlen(hdr))) { sock_close(c); return; }
        // push a fresh snapshot every 100ms until the client goes away
        while (g_running.load()) {
            std::string msg = "data: " + snapshot_json() + "\n\n";
            if (!send_all(c, msg.data(), msg.size())) break;   // client gone -> exit
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        sock_close(c);
        return;
    }

    // default: serve the page
    std::string body = (path == "/" || path == "/index.html") ? kPage
                       : std::string("not found");
    std::string resp = "HTTP/1.1 " + std::string(path == "/" || path == "/index.html" ? "200 OK" : "404 Not Found") +
        "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    send_all(c, resp.data(), resp.size());
    sock_close(c);
}

int main(int argc, char** argv) {
    int port = 8080;
    unsigned rate = 30000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)std::atoi(argv[++i]);
    }

    net_init();
    socket_t srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { std::printf("socket() failed\n"); return 1; }
    int yes = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only
    addr.sin_port = htons((unsigned short)port);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0) { std::printf("bind(%d) failed\n", port); return 1; }
    ::listen(srv, 16);

    std::thread matcher(matching_thread, rate);

    std::printf("Mini-Exchange visualizer:  http://127.0.0.1:%d/\n", port);
    std::printf("SSE endpoint:              http://127.0.0.1:%d/events\n", port);
    std::printf("(matching runs full speed on its own thread; snapshots are pushed off the hot path)\n");
    std::printf("Ctrl+C to stop.\n");

    for (;;) {
        socket_t c = ::accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(handle_client, c).detach();
    }
    g_running.store(false);
    matcher.join();
    sock_close(srv);
    return 0;
}
