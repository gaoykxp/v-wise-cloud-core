/**********************************************************************************************************************
    > File: net.cpp
    > 云端跨平台网络层实现
**********************************************************************************************************************/
#include "net.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace vwise {
namespace cloud {

#ifdef _WIN32

bool net_init() {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
}
void net_cleanup() { WSACleanup(); }

static void sock_err(const char *what) {
    fprintf(stderr, "[net] %s failed: wsaerr=%d\n", what, WSAGetLastError());
}

static sockaddr_in ep_to_addr(const Endpoint &e) {
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    a.sin_addr.s_addr = (e.ip.empty() || e.ip == "*") ? htonl(INADDR_ANY) : inet_addr(e.ip.c_str());
    return a;
}
static Endpoint addr_to_ep(const sockaddr_in &a) {
    Endpoint e;
    e.valid = true;
    e.port = ntohs(a.sin_port);
    e.ip = inet_ntoa(a.sin_addr);
    return e;
}

bool UdpSocket::bind(const std::string &ip, uint16_t port, const std::string & /*device*/) {
    if (valid()) close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ == k_invalid) { sock_err("socket"); return false; }
    /* 允许端口复用（测试期反复重启） */
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    sockaddr_in a = ep_to_addr({ip, port, true});
    if (::bind(fd_, (sockaddr *)&a, sizeof(a)) != 0) {
        sock_err("bind");
        close();
        return false;
    }
    return true;
}

bool UdpSocket::set_recv_timeout_ms(int ms) {
    DWORD t = (DWORD)ms;
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t)) == 0;
}

void UdpSocket::close() {
    if (valid()) { closesocket(fd_); fd_ = k_invalid; }
}

int UdpSocket::send_to(const void *buf, int len, const Endpoint &to) {
    sockaddr_in a = ep_to_addr(to);
    return ::sendto(fd_, (const char *)buf, len, 0, (sockaddr *)&a, sizeof(a));
}

int UdpSocket::recv_from(void *buf, int cap, Endpoint &from) {
    sockaddr_in a;
    socklen_t alen = sizeof(a);
    int n = ::recvfrom(fd_, (char *)buf, cap, 0, (sockaddr *)&a, &alen);
    if (n <= 0) return n;
    from = addr_to_ep(a);
    return n;
}

bool TcpSocket::connect(const std::string &host, uint16_t port, int timeout_ms) {
    if (valid()) close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ == k_invalid) { sock_err("socket"); return false; }
    /* 简化：Windows 使用阻塞 connect（局域/本机 mosquitto 足够） */
    u_long nb = 1;
    ioctlsocket(fd_, FIONBIO, &nb);
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(host.c_str());
    int r = ::connect(fd_, (sockaddr *)&a, sizeof(a));
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        sock_err("connect");
        close();
        return false;
    }
    /* 等待可写（connect 完成） */
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd_, &w);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (::select(0, NULL, &w, NULL, &tv) <= 0) {
        fprintf(stderr, "[net] tcp connect timeout (%s:%u)\n", host.c_str(), port);
        close();
        return false;
    }
    nb = 0;
    ioctlsocket(fd_, FIONBIO, &nb); /* 恢复阻塞模式 */
    return true;
}

void TcpSocket::set_recv_timeout_ms(int ms) {
    DWORD t = (DWORD)ms;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
}
void TcpSocket::close() { if (valid()) { closesocket(fd_); fd_ = k_invalid; } }

int TcpSocket::send_all(const void *buf, int len) {
    const char *p = (const char *)buf;
    int left = len;
    while (left > 0) {
        int n = ::send(fd_, p, left, 0);
        if (n <= 0) return -1;
        p += n;
        left -= n;
    }
    return len;
}
int TcpSocket::recv(void *buf, int cap) {
    return ::recv(fd_, (char *)buf, cap, 0);
}

/* ---------------- Windows TCP server ---------------- */
bool TcpServerSocket::listen(uint16_t port) {
    if (valid()) close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ == k_invalid) { sock_err("socket"); return false; }
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, (sockaddr *)&a, sizeof(a)) != 0) {
        sock_err("bind");
        close();
        return false;
    }
    if (::listen(fd_, 16) != 0) {
        sock_err("listen");
        close();
        return false;
    }
    return true;
}

void TcpServerSocket::close() {
    if (valid()) { closesocket(fd_); fd_ = k_invalid; }
}

bool TcpServerSocket::accept(TcpSocket &out, int nodelay) {
    if (!valid()) return false;
    SOCKET c = ::accept(fd_, nullptr, nullptr);
    if (c == INVALID_SOCKET) return false;
    if (nodelay) {
        int nd = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof(nd));
    }
    out.close();
    out.fd_ = c;
    return true;
}

#else /* ============================ POSIX ============================ */

bool net_init() { return true; }
void net_cleanup() {}

static void sock_err(const char *what) { perror(what); }

static sockaddr_in ep_to_addr(const Endpoint &e) {
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    a.sin_addr.s_addr = (e.ip.empty() || e.ip == "*") ? htonl(INADDR_ANY) : inet_addr(e.ip.c_str());
    return a;
}
static Endpoint addr_to_ep(const sockaddr_in &a) {
    Endpoint e;
    e.valid = true;
    e.port = ntohs(a.sin_port);
    char ip[64];
    if (inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip))) e.ip = ip;
    return e;
}

bool UdpSocket::bind(const std::string &ip, uint16_t port, const std::string &device) {
    if (valid()) close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) { sock_err("socket"); return false; }
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (!device.empty()) {
        /* SO_BINDTODEVICE：绑定出接口（需 root/CAP_NET_RAW），云端一般不使用，保留能力 */
        if (setsockopt(fd_, SOL_SOCKET, SO_BINDTODEVICE, device.c_str(), (socklen_t)device.size()) != 0)
            sock_err("SO_BINDTODEVICE");
    }
    sockaddr_in a = ep_to_addr({ip, port, true});
    if (::bind(fd_, (sockaddr *)&a, sizeof(a)) != 0) {
        sock_err("bind");
        close();
        return false;
    }
    return true;
}

bool UdpSocket::set_recv_timeout_ms(int ms) {
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

void UdpSocket::close() {
    if (valid()) { ::close(fd_); fd_ = -1; }
}

int UdpSocket::send_to(const void *buf, int len, const Endpoint &to) {
    sockaddr_in a = ep_to_addr(to);
    return (int)::sendto(fd_, buf, len, 0, (sockaddr *)&a, sizeof(a));
}

int UdpSocket::recv_from(void *buf, int cap, Endpoint &from) {
    sockaddr_in a;
    socklen_t alen = sizeof(a);
    int n = (int)::recvfrom(fd_, buf, cap, 0, (sockaddr *)&a, &alen);
    if (n <= 0) return n;
    from = addr_to_ep(a);
    return n;
}

bool TcpSocket::connect(const std::string &host, uint16_t port, int timeout_ms) {
    if (valid()) close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { sock_err("socket"); return false; }
    /* 非阻塞 connect + select 等待 */
    int fl = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(host.c_str());
    int r = ::connect(fd_, (sockaddr *)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) {
        sock_err("connect");
        close();
        return false;
    }
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd_, &w);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (::select(fd_ + 1, NULL, &w, NULL, &tv) <= 0) {
        fprintf(stderr, "[net] tcp connect timeout (%s:%u)\n", host.c_str(), port);
        close();
        return false;
    }
    fcntl(fd_, F_SETFL, fl); /* 恢复阻塞 */
    return true;
}

void TcpSocket::set_recv_timeout_ms(int ms) {
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
void TcpSocket::close() { if (valid()) { ::close(fd_); fd_ = -1; } }

int TcpSocket::send_all(const void *buf, int len) {
    const char *p = (const char *)buf;
    int left = len;
    while (left > 0) {
        ssize_t n = ::send(fd_, p, left, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        left -= (int)n;
    }
    return len;
}
int TcpSocket::recv(void *buf, int cap) {
    return (int)::recv(fd_, buf, cap, 0);
}

/* ---------------- POSIX TCP server ---------------- */
bool TcpServerSocket::listen(uint16_t port) {
    if (valid()) close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { sock_err("socket"); return false; }
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd_, (sockaddr *)&a, sizeof(a)) != 0) {
        sock_err("bind");
        close();
        return false;
    }
    if (::listen(fd_, 16) != 0) {
        sock_err("listen");
        close();
        return false;
    }
    return true;
}

void TcpServerSocket::close() {
    if (valid()) { ::close(fd_); fd_ = -1; }
}

bool TcpServerSocket::accept(TcpSocket &out, int nodelay) {
    if (!valid()) return false;
    int c = ::accept(fd_, nullptr, nullptr);
    if (c < 0) return false;
    if (nodelay) {
        int nd = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof(nd));
    }
    out.close();
    out.fd_ = c;
    return true;
}

#endif /* _WIN32 */

Endpoint parse_endpoint(const std::string &s, uint16_t default_port) {
    Endpoint e;
    e.valid = false;
    std::string ip = s;
    uint16_t port = default_port;
    size_t p = s.rfind(':');
    if (p != std::string::npos && s.find(':') == p) {
        ip = s.substr(0, p);
        port = (uint16_t)atoi(s.substr(p + 1).c_str());
    }
    if (ip.empty()) return e;
    e.ip = ip;
    e.port = port;
    e.valid = true;
    return e;
}

} // namespace cloud
} // namespace vwise
