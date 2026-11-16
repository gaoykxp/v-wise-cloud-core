/**********************************************************************************************************************
    > File: mini_mqtt.cpp
    > 轻量 MQTT v3.1.1 客户端实现（QoS0）
    > 报文：CONNECT/CONNACK、SUBSCRIBE/SUBACK、PUBLISH(qos0)、PINGREQ/PINGRESP。
**********************************************************************************************************************/
#include "mini_mqtt.h"
#include "logger.h"
#include <chrono>
#include <cstring>
#include <vector>

namespace vwise {
namespace cloud {

/* 剩余长度 varint 编码 */
static void encode_remaining_len(std::vector<uint8_t> &out, size_t len) {
    do {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) b |= 0x80;
        out.push_back(b);
    } while (len > 0);
}

static void encode_str(std::vector<uint8_t> &out, const std::string &s) {
    uint16_t n = (uint16_t)s.size();
    out.push_back((uint8_t)(n >> 8));
    out.push_back((uint8_t)(n & 0xFF));
    out.insert(out.end(), s.begin(), s.end());
}

MiniMqtt::MiniMqtt(std::string host, uint16_t port, std::string client_id)
    : host_(std::move(host)), port_(port), client_id_(std::move(client_id)) {}

void MiniMqtt::add_subscribe(const std::string &topic_filter) {
    filters_.push_back(topic_filter);
}

bool MiniMqtt::write_packet(const std::vector<uint8_t> &pkt) {
    if (!sock_.valid()) return false;
    return sock_.send_all(pkt.data(), (int)pkt.size()) == (int)pkt.size();
}

bool MiniMqtt::send_connect() {
    /* CONNECT: 头 + 协议名/级别 + 标志 + keepalive + client id */
    std::vector<uint8_t> body;
    encode_str(body, "MQTT");
    body.push_back(4);                 /* protocol level 4 = 3.1.1 */
    body.push_back(0x02);              /* clean session */
    body.push_back(0x00); body.push_back(0x3C); /* keepalive 60s */
    encode_str(body, client_id_);

    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    encode_remaining_len(pkt, body.size());
    pkt.insert(pkt.end(), body.begin(), body.end());
    if (!write_packet(pkt)) return false;

    /* 等 CONNACK */
    uint8_t buf[16];
    int n = sock_.recv(buf, sizeof(buf));
    if (n < 4 || buf[0] != 0x20 || buf[3] != 0) {
        LOGE("mqtt CONNACK invalid (n=%d byte0=0x%02X rc=%d)", n, (n > 0 ? buf[0] : 0),
             (n > 3 ? buf[3] : -1));
        return false;
    }
    return true;
}

bool MiniMqtt::send_subscribe_all() {
    bool ok = true;
    for (auto &f : filters_) {
        std::vector<uint8_t> body;
        uint16_t pid = ++next_pkt_id_;
        body.push_back((uint8_t)(pid >> 8));
        body.push_back((uint8_t)(pid & 0xFF));
        encode_str(body, f);
        body.push_back(0x00); /* qos0 */
        std::vector<uint8_t> pkt;
        pkt.push_back(0x82);
        encode_remaining_len(pkt, body.size());
        pkt.insert(pkt.end(), body.begin(), body.end());
        if (!write_packet(pkt)) { ok = false; continue; }
        /* 等 SUBACK（粗暴等待，控制面低频足够） */
        uint8_t buf[64];
        int n = sock_.recv(buf, sizeof(buf));
        if (n < 5 || buf[0] != 0x90) {
            LOGW("mqtt SUBACK not received for '%s' (n=%d)", f.c_str(), n);
            ok = false;
        } else {
            LOGI("mqtt subscribed: %s", f.c_str());
        }
    }
    return ok;
}

void MiniMqtt::send_ping() {
    uint8_t ping[2] = {0xC0, 0x00};
    sock_.send_all(ping, 2);
}

bool MiniMqtt::publish(const std::string &topic, const std::string &payload) {
    if (!connected_.load()) return false;
    std::vector<uint8_t> body;
    encode_str(body, topic);
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<uint8_t> pkt;
    pkt.push_back(0x30); /* PUBLISH qos0 */
    encode_remaining_len(pkt, body.size());
    pkt.insert(pkt.end(), body.begin(), body.end());
    return write_packet(pkt);
}

void MiniMqtt::handle_incoming(uint8_t *buf, int len) {
    size_t pos = 0;
    while (pos < (size_t)len) {
        uint8_t type = buf[pos] & 0xF0;
        if (type != 0x30) { pos = (size_t)len; break; } /* 非 PUBLISH：丢弃剩余缓冲 */
        /* 解析剩余长度 */
        size_t rem = 0;
        int shift = 0;
        size_t p = pos + 1;
        while (p < (size_t)len) {
            uint8_t b = buf[p++];
            rem |= (size_t)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }
        if (p + 2 > (size_t)len) return;
        uint16_t tlen = (uint16_t)((buf[p] << 8) | buf[p + 1]);
        p += 2;
        if (p + tlen > (size_t)len) return;
        std::string topic((const char *)buf + p, tlen);
        p += tlen;
        size_t plen = rem - 2 - tlen;
        if (p + plen > (size_t)len) return;
        std::string payload((const char *)buf + p, plen);
        if (cb_) cb_(topic, payload);
        pos = p + plen;
    }
}

void MiniMqtt::run_loop() {
    /* keepalive 策略：15s 无入包则 PINGREQ */
    sock_.set_recv_timeout_ms(15000);
    auto last_ping = std::chrono::steady_clock::now();
    while (running_.load()) {
        uint8_t buf[4096];
        int n = sock_.recv(buf, sizeof(buf));
        if (n > 0) {
            handle_incoming(buf, n);
        } else if (n == 0) {
            /* 连接被对端关闭 */
            LOGW("mqtt: connection closed by broker");
            connected_.store(false);
            return;
        } else {
            /* 超时或错误：超时(>0?) —— Windows recv 返回 -1 + WSAETIMEDOUT，Linux -1 + EAGAIN */
            auto now = std::chrono::steady_clock::now();
            if (now - last_ping > std::chrono::seconds(20)) {
                send_ping();
                last_ping = now;
            }
        }
    }
}

bool MiniMqtt::start() {
    if (running_.load()) return true;
    if (!sock_.connect(host_, port_, 5000)) {
        LOGE("mqtt: connect %s:%u failed", host_.c_str(), port_);
        return false;
    }
    if (!send_connect()) {
        LOGE("mqtt: CONNECT handshake failed");
        sock_.close();
        return false;
    }
    connected_.store(true);
    send_subscribe_all();
    running_.store(true);
    th_ = std::thread(&MiniMqtt::run_loop, this);
    LOGI("mqtt: connected to %s:%u", host_.c_str(), port_);
    return true;
}

void MiniMqtt::stop() {
    running_.store(false);
    if (th_.joinable()) {
        /* 阻塞 recv 最多 15s；先关 socket 使 recv 立刻返回 */
        sock_.close();
        th_.join();
    }
    connected_.store(false);
}

} // namespace cloud
} // namespace vwise
