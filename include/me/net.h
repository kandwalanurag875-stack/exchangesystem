// net.h — a tiny cross-platform TCP helper (Winsock / BSD sockets) with
// length-framed message I/O. Used by the Raft TCP transport (Milestone 7).
//
// Include this BEFORE anything that might pull in <windows.h>: on Windows,
// winsock2.h must precede windows.h, and NOMINMAX keeps windows.h's min/max
// macros from clobbering std::min/std::max in the engine headers.
#ifndef ME_NET_H
#define ME_NET_H

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace me {

#if defined(_WIN32)
using socket_t = SOCKET;
inline int  net_close(socket_t s) { return closesocket(s); }
inline void net_init() { static WSADATA w; static bool done = false; if (!done) { WSAStartup(MAKEWORD(2, 2), &w); done = true; } }
inline bool net_valid(socket_t s) { return s != INVALID_SOCKET; }
#else
using socket_t = int;
static const socket_t INVALID_SOCKET = -1;
inline int  net_close(socket_t s) { return ::close(s); }
inline void net_init() {}
inline bool net_valid(socket_t s) { return s >= 0; }
#endif

// Listen on 127.0.0.1:port. Returns INVALID_SOCKET on failure.
inline socket_t net_listen(int port, int backlog = 16) {
    net_init();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!net_valid(s)) return INVALID_SOCKET;
    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port);
    if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0) { net_close(s); return INVALID_SOCKET; }
    if (::listen(s, backlog) != 0) { net_close(s); return INVALID_SOCKET; }
    return s;
}

// Connect to host:port. Returns INVALID_SOCKET on failure (caller retries).
inline socket_t net_connect(const std::string& host, int port) {
    net_init();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!net_valid(s)) return INVALID_SOCKET;
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // default to loopback
    }
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) { net_close(s); return INVALID_SOCKET; }
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    return s;
}

inline bool net_send_all(socket_t s, const void* p, std::size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        int k = ::send(s, c, (int)n, 0);
        if (k <= 0) return false;
        c += k; n -= (std::size_t)k;
    }
    return true;
}

inline bool net_recv_all(socket_t s, void* p, std::size_t n) {
    char* c = static_cast<char*>(p);
    while (n) {
        int k = ::recv(s, c, (int)n, 0);
        if (k <= 0) return false;
        c += k; n -= (std::size_t)k;
    }
    return true;
}

// Frame = 4-byte little-endian length prefix + payload.
inline bool net_send_framed(socket_t s, const std::vector<std::uint8_t>& body) {
    std::uint8_t hdr[4];
    std::uint32_t n = (std::uint32_t)body.size();
    hdr[0] = n & 0xFF; hdr[1] = (n >> 8) & 0xFF; hdr[2] = (n >> 16) & 0xFF; hdr[3] = (n >> 24) & 0xFF;
    return net_send_all(s, hdr, 4) && (body.empty() || net_send_all(s, body.data(), body.size()));
}

inline bool net_recv_framed(socket_t s, std::vector<std::uint8_t>& out) {
    std::uint8_t hdr[4];
    if (!net_recv_all(s, hdr, 4)) return false;
    std::uint32_t n = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((std::uint32_t)hdr[3] << 24);
    out.resize(n);
    return n == 0 || net_recv_all(s, out.data(), n);
}

}  // namespace me

#endif  // ME_NET_H
