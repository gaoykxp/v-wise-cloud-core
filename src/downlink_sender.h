/**********************************************************************************************************************
    > File: downlink_sender.h / downlink_sender.cpp 合并
    > 下行发送器（P2：择优/双发，DupHdr 封装）：
    *   - mode: off | best | dual（best 走最活跃锚点；dual 每锚点各发一份同 seq 副本）
    *   - 目标端点：每锚点独立学习（该锚点最近收到的终端 DATA 源地址，PROBE 兜底）
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_DOWNLINK_SENDER_H
#define VWISE_CLOUD_DOWNLINK_SENDER_H

#include "anchor_server.h"
#include <atomic>
#include <functional>
#include <thread>

namespace vwise {
namespace cloud {

enum class DlMode { OFF = 0, BEST = 1, DUAL = 2, AUTO = 3 };

class DownlinkSender {
public:
    DownlinkSender(std::vector<AnchorServer *> anchors) : anchors_(std::move(anchors)) {}
    ~DownlinkSender() { stop(); }

    void configure(uint32_t flow_id, int payload_bytes, double rate_pps);
    void set_mode(DlMode m) { mode_.store(m); }
    DlMode mode() const { return mode_.load(); }

    /* 终端在线检查（CloudServer 注入）：离线时暂停发送（不计数），避免
     * 向失效端点持续发包污染验收统计 */
    void set_online_check(std::function<bool()> fn) { online_check_ = std::move(fn); }

    bool start();
    void stop();

    uint64_t sent_total() const { return sent_.load(); }
    uint64_t sent_by_anchor(int i) const { return sent_anchor_[i & 7].load(); }

    /* auto 模式决策依据：锚点最近收包时间越新越优 */
    int best_anchor_idx() const;

private:
    void run();
    Endpoint target_of(int anchor_idx);

    std::vector<AnchorServer *> anchors_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<DlMode> mode_{DlMode::OFF};

    /* best 模式锚点粘滞：每 3s 重估一次，避免逐包选锚在两锚点间抖动
     * 造成跨 socket 乱序（实测 oos≈7%） */
    int cur_best_ = -1;
    std::chrono::steady_clock::time_point best_eval_at_;

    uint32_t flow_id_ = 0x5100;
    int payload_bytes_ = 200;
    double rate_pps_ = 50.0;
    uint32_t seq_ = 0;
    std::function<bool()> online_check_;
    std::atomic<uint64_t> sent_{0};
    std::atomic<uint64_t> sent_anchor_[8] = {};
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_DOWNLINK_SENDER_H
