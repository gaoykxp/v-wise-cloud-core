/**********************************************************************************************************************
    > File: mini_mqtt.h
    > 轻量 MQTT v3.1.1 客户端（QoS0-only）：云端控制面接入 mosquitto/EMQX。
    > 说明：生产化阶段建议替换为 paho.mqtt.c（接口已收敛在 MiniMqtt 类），本实现仅面向
    >       链路状态订阅/模式指令发布这类低频小报文场景。
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_MINI_MQTT_H
#define VWISE_CLOUD_MINI_MQTT_H

#include "net.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace vwise {
namespace cloud {

class MiniMqtt {
public:
    using OnMessage = std::function<void(const std::string &topic, const std::string &payload)>;

    MiniMqtt(std::string host, uint16_t port, std::string client_id);
    ~MiniMqtt() { stop(); }

    void set_on_message(OnMessage cb) { cb_ = std::move(cb); }
    void add_subscribe(const std::string &topic_filter);

    bool start();   /* 连接 + 订阅 + 保活/收包线程 */
    void stop();
    bool publish(const std::string &topic, const std::string &payload); /* QoS0 */

private:
    /* 内部 API：无锁，供收包线程/外部 start 阶段调用 */
    bool send_connect();
    bool send_subscribe_all();
    void send_ping();
    void run_loop();

    bool write_packet(const std::vector<uint8_t> &pkt);
    void handle_incoming(uint8_t *buf, int len);

    std::string host_;
    uint16_t port_;
    std::string client_id_;
    std::vector<std::string> filters_;
    TcpSocket sock_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    OnMessage cb_;
    uint16_t next_pkt_id_ = 1;
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_MINI_MQTT_H
