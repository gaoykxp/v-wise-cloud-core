/**********************************************************************************************************************
    > File: anchor_server.h
    > 云端锚点服务器（每锚点一个实例，模拟一个 PoP 接收点）：
    >   - PROBE_REQ → PROBE_ACK（回显 t_send；带宽 train 末包携带云侧实测 rx_rate）
    >   - DATA_UP   → 全局 (flow_id,seq) 去重（先到先得），统计每锚点先到/重复
    *   - STATS_REQ → STATS_ACK（瞬时计数器对账）
    *   - 端点学习：记录每锚点最近一次收到终端包的源地址（DATA 优先，PROBE 兜底）供下行寻址
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_ANCHOR_SERVER_H
#define VWISE_CLOUD_ANCHOR_SERVER_H

#include "dedup.h"
#include "net.h"
#include "vwise_dup_protocol.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace vwise {
namespace cloud {

struct TerminalStats; /* 汇聚层注入（terminal_stats.h） */

/* 跨锚点共享的上行统计（选收去重发生在这一层） */
struct UplinkStats {
    std::mutex mtx;
    FlowDedup global_dedup;          /* 全局去重（跨锚点） */
    uint64_t rx_unique = 0;          /* 全局去重后唯一包数 */
    uint64_t rx_dup = 0;             /* 全局重复丢弃数 */
    uint64_t first_by_anchor[8] = {0}; /* 每锚点“先到”计数（该包本锚点先于其他锚点到达） */
    uint64_t oos_events = 0;         /* 全局乱序事件（seq 落后窗口） */
    std::atomic<bool> epoch_dirty{false}; /* 终端模式变化 → 看板验收 epoch 需重置 */

    void snapshot(uint64_t out[4]) {
        std::lock_guard<std::mutex> lk(mtx);
        out[0] = rx_unique;
        out[1] = rx_dup;
        out[2] = first_by_anchor[0];
        out[3] = first_by_anchor[1];
    }

    void clear_counters() {
        std::lock_guard<std::mutex> lk(mtx);
        rx_unique = rx_dup = oos_events = 0;
        for (int i = 0; i < 8; ++i) first_by_anchor[i] = 0;
        global_dedup.clear();
    }
};

class AnchorServer {
public:
    AnchorServer(int id, const Endpoint &listen) : id_(id), listen_(listen) {}
    ~AnchorServer() { stop(); }

    /* stats：上行选收统计（跨锚点共享）；term：终端状态汇聚（可空，看板用） */
    bool start(std::shared_ptr<UplinkStats> stats, std::shared_ptr<TerminalStats> term = nullptr);
    void stop();

    int id() const { return id_; }
    bool send_to(const void *buf, int len, const Endpoint &to) { return sock_.send_to(buf, len, to) > 0; }

    /* 终端端点（下行寻址）：DATA 源地址优先，PROBE 源地址兜底 */
    Endpoint terminal_data_endpoint();
    Endpoint terminal_probe_endpoint();

    uint64_t rx_packets() const { return rx_pkts_.load(); }
    uint64_t probe_acks() const { return probe_acks_.load(); }
    uint64_t last_rx_ns() const { return last_rx_.load(); }

    void clear_counters();

private:
    void run();
    void handle_probe_req(const VwPktHdr &h, const Endpoint &from, int pkt_len);
    void handle_data_up(const VwPktHdr &h, const Endpoint &from);
    void handle_stats_req(const VwPktHdr &h, const Endpoint &from);
    void handle_term_stats(const VwPktHdr &h, const Endpoint &from, int pkt_len);

    int id_;
    Endpoint listen_;
    std::shared_ptr<UplinkStats> stats_;
    std::shared_ptr<TerminalStats> term_;   /* 可空 */
    UdpSocket sock_;
    std::thread th_;
    std::atomic<bool> running_{false};

    std::mutex ep_mtx_;
    Endpoint data_ep_, probe_ep_;   /* 学习到的终端端点 */
    bool has_data_ep_ = false, has_probe_ep_ = false;

    /* train 计量（每锚点独立） */
    uint32_t cur_train_id_ = 0;
    uint64_t train_bytes_ = 0;
    uint64_t train_first_ns_ = 0;

    /* 本锚点每流独立去重表（统计锚点自身丢包后的投递率） */
    FlowDedup local_dedup_;

    std::atomic<uint64_t> rx_pkts_{0};
    std::atomic<uint64_t> probe_acks_{0};
    std::atomic<uint64_t> last_rx_{0};
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_ANCHOR_SERVER_H
