/**********************************************************************************************************************
    > File: cloud_server.h
    > 云端网关组装层：锚点集群 + 下行发送 + MQTT 控制面 + 终端状态汇聚 + Web 看板数据。
    * Web 看板（v2）新增：
    *   - TerminalStats：TERM_STATS 包 / MQTT 双通道合一
    *   - 事件环形缓冲（模式切换/断链/控制操作/启动）
    *   - 时序环形缓冲（10min，每 stats 周期采样）
    *   - 验收判定（P0-P2 测试标准实时计算）
    *   - 控制动作：dl_mode / clear_stats / term_mode(CTRL 下发) / train
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_SERVER_H
#define VWISE_CLOUD_SERVER_H

#include "anchor_server.h"
#include "cloud_config.h"
#include "downlink_sender.h"
#include "mini_json.h"
#include "mini_mqtt.h"
#include "terminal_stats.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace vwise {
namespace cloud {

/* 看板事件 */
struct WebEvent {
    uint64_t ts_ms = 0;   /* 墙钟 ms */
    std::string type;    /* mode_change | link_down | link_up | control | server | train */
    std::string detail;
};

/* 时序采样点（stats 周期） */
struct HistSample {
    uint64_t t_ms = 0;       /* 墙钟 ms */
    double dup_ratio = 0;    /* 上行重复占比 %（本周期增量） */
    double rtt_ms[2] = {-1, -1};  /* 终端上报每链路 RTT */
    double thr_up0 = 0;      /* 锚点0 上行 unique 吞吐 pps */
    double thr_up1 = 0;      /* 锚点1 上行 unique 吞吐 pps */
    double dl_pps = 0;       /* 下行发送 pps */
    double term_dup_ratio = -1; /* 终端下行去重重复占比 % */
};

class CloudServer {
public:
    explicit CloudServer(const Config &cfg);
    ~CloudServer() { stop(); }

    bool start();
    void stop();

    /* ---- 控制面（CLI / 看板共用） ---- */
    void set_dl_mode(DlMode m);
    void clear_stats();
    bool send_ctrl(const std::string &json); /* VWD_T_CTRL → 终端探测端点 */

    TerminalStats &term() { return *term_; }
    std::shared_ptr<TerminalStats> term_ptr() { return term_; }
    DownlinkSender &downlink() { return *dl_; }
    std::shared_ptr<UplinkStats> uplink_stats() { return stats_; }
    const std::vector<AnchorServer *> &anchors() const { return anchor_ptrs_; }
    const Config &config() const { return cfg_; }

    /* ---- 看板数据出口 ---- */
    std::string build_overview_json();          /* /api/overview 全量快照 */
    std::string build_events_json(int n);       /* /api/events */
    std::string build_history_json();           /* /api/history */
    std::string build_prometheus_text();        /* /metrics */
    /* 看板控制动作：{"action":"dl_mode|clear_stats|term_mode|train", "mode":"..."} */
    bool handle_ctl(const std::string &body, std::string &err);

    static const char *dl_mode_name(DlMode m) {
        switch (m) {
            case DlMode::OFF: return "off";
            case DlMode::BEST: return "best";
            case DlMode::DUAL: return "dual";
            case DlMode::AUTO: return "auto";
        }
        return "?";
    }

    void decide_auto_mode();

private:
    void status_loop();
    void on_mqtt_message(const std::string &topic, const std::string &payload);
    void add_event(const std::string &type, const std::string &detail);

    /* 验收判定：追加 {id,desc,value,status} 数组元素到 j 字符串（含逗号管理） */
    void append_acceptance(std::string &j);
    void reset_epoch_locked();

    Config cfg_;
    std::shared_ptr<UplinkStats> stats_;
    std::vector<std::unique_ptr<AnchorServer>> anchors_;
    std::vector<AnchorServer *> anchor_ptrs_;
    std::unique_ptr<DownlinkSender> dl_;
    std::unique_ptr<MiniMqtt> mqtt_;
    std::shared_ptr<TerminalStats> term_;   /* 注入各锚点（TERM_STATS 汇聚） */

    std::thread status_th_;
    std::atomic<bool> running_{false};

    /* MQTT 解析暂存（on_mqtt_message 内使用后并入 term_） */
    std::string term_imei_;

    /* 事件环形缓冲 */
    std::mutex ev_mtx_;
    std::deque<WebEvent> events_;

    /* 时序环形缓冲（10min @2s = 600 点） */
    std::mutex hist_mtx_;
    std::deque<HistSample> history_;

    /* 周期增量（吞吐/dup 率计算） */
    uint64_t last_unique_ = 0, last_dup_ = 0;
    uint64_t last_first_[8] = {0};
    uint64_t last_dl_sent_ = 0;
    uint64_t last_term_rx_ = 0;

    /* 验收 epoch 基线（模式切换/清零时重置） */
    uint64_t epoch_unique_ = 0, epoch_dup_ = 0, epoch_first_[8] = {0};
    uint64_t epoch_anchor_rx_[8] = {0}; /* per-锚点收包基线（P1-3 epoch 差分） */
    uint64_t epoch_term_rx_ = 0, epoch_term_unique_ = 0;
    std::string epoch_term_mode_;  /* epoch 起始时终端模式 */
    std::string epoch_dl_mode_;    /* epoch 起始时下行模式 */
    /* 终端基线延迟采样：epoch 重置时 TERM_STATS 快照最多滞后 2s，
     * 直接取基线会让 P2 项在整个 epoch 带恒定负偏差——
     * 置 pending，等重置后到达的第一帧新鲜 TERM_STATS 再取基线 */
    bool epoch_term_pending_ = false;
    uint64_t epoch_reset_ns_ = 0;

    /* 匹配对采样：TERM_STATS 到达瞬间同步读云端下行计数（on_stats 回调），
     * 终端侧(最多滞后2s的快照)与云端侧(实时)在同一定时点上取值，
     * 消除 P2 项两源时基错配的系统性偏差。终端静默后匹配对自然冻结（离线窗口闭合） */
    std::mutex match_mtx_;
    uint64_t match_term_rx_ = 0, match_term_unique_ = 0;
    uint64_t match_dl_copies_ = 0, match_dl_sent_ = 0;
    uint64_t epoch_match_dl_copies_ = 0, epoch_match_dl_sent_ = 0;

    /* 终端模式变化事件检测 */
    std::string last_term_mode_seen_;
    std::string last_term_link_alive_[2]; /* "true"/"false" 检测边沿 */
    bool last_term_online_ = false;       /* 终端上线沿检测 → epoch 重置（验收窗口对齐） */
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_SERVER_H
