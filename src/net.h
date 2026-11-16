/**********************************************************************************************************************
    > File: net.h
    > 云端跨平台网络层（Windows Winsock / Linux POSIX 双端通用）
    > P0-P2 实施阶段：阻塞式 UDP/TCP + SO_RCVTIMEO，线程模型为主，不依赖 poll/epoll 差异。
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_NET_H
#define VWISE_CLOUD_NET_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#endif

#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

namespace vwise {
namespace cloud {

/* 单调时钟纳秒（全工程共享定义） */
static inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static inline uint64_t now_ms_epoch() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/* Winsock 全局初始化（Windows 进程调用一次；Linux 空实现） */
bool net_init();
void net_cleanup();

/* 解析 "ip:port" / "ip" 列表（逗号分隔），port 为默认端口 */
struct Endpoint {
    std::string ip;
    uint16_t port = 0;
    bool valid = false;
};
Endpoint parse_endpoint(const std::string &s, uint16_t default_port);

/* UDP 套接字：阻塞收发 + 收包超时 */
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() { close(); }

    /* ip 为空 → INADDR_ANY；device 仅 Linux 有效（SO_BINDTODEVICE，云端一般不用） */
    bool bind(const std::string &ip, uint16_t port, const std::string &device = "");
    bool set_recv_timeout_ms(int ms);
    void close();

    int send_to(const void *buf, int len, const Endpoint &to);
    /* 返回收包长度，<=0 为超时/错误 */
    int recv_from(void *buf, int cap, Endpoint &from);

    bool valid() const { return fd_ != k_invalid; }

private:
#ifdef _WIN32
    typedef SOCKET raw_fd_t;
    static const raw_fd_t k_invalid = INVALID_SOCKET;
#else
    typedef int raw_fd_t;
    static const raw_fd_t k_invalid = -1;
#endif
    raw_fd_t fd_ = k_invalid;
};

/* TCP 套接字（MiniMqtt 用）：connect / 阻塞收发 + 超时 */
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close(); }

    bool connect(const std::string &host, uint16_t port, int timeout_ms);
    void set_recv_timeout_ms(int ms);
    void close();

    int send_all(const void *buf, int len);
    /* 返回收包长度，<=0 为超时/错误 */
    int recv(void *buf, int cap);

    bool valid() const { return fd_ != k_invalid; }

private:
    friend class TcpServerSocket; /* accept 直接注入 fd */

#ifdef _WIN32
    typedef SOCKET raw_fd_t;
    static const raw_fd_t k_invalid = INVALID_SOCKET;
#else
    typedef int raw_fd_t;
    static const raw_fd_t k_invalid = -1;
#endif
    raw_fd_t fd_ = k_invalid;
};

/* TCP 服务端（Web 看板用）：listen/accept；accept 返回已连接的 TcpSocket。
 * stop() 关闭监听 fd 使阻塞 accept 退出。 */
class TcpServerSocket {
public:
    TcpServerSocket() = default;
    ~TcpServerSocket() { close(); }

    bool listen(uint16_t port);
    void close();
    bool valid() const { return fd_ != k_invalid; }

    /* 阻塞 accept；返回 false 表示监听已关闭/出错 */
    bool accept(TcpSocket &out, int nodelay = 1);

private:
#ifdef _WIN32
    typedef SOCKET raw_fd_t;
    static const raw_fd_t k_invalid = INVALID_SOCKET;
#else
    typedef int raw_fd_t;
    static const raw_fd_t k_invalid = -1;
#endif
    raw_fd_t fd_ = k_invalid;
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_NET_H
