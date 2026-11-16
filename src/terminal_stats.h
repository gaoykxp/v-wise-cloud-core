/**********************************************************************************************************************
    > File: terminal_stats.h
    > 终端状态汇聚（Web 看板数据源）：两通道合一，后到覆盖：
    *   1. VWD_T_TERM_STATS 包（无 broker 联调，2s 周期）
    *   2. MQTT vwise/+/link/status/up（有 broker 联调）
    * 线程安全：所有读写经 mtx。
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_TERMINAL_STATS_H
#define VWISE_CLOUD_TERMINAL_STATS_H

#include "net.h"
#include "vwise_dup_protocol.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vwise {
namespace cloud {

static inline const char *vwd_mode_name(int m) {
    switch (m) {
        case VWD_MODE_SINGLE_BEST: return "single_best";
        case VWD_MODE_DUAL_DUP: return "dual_dup";
        case VWD_MODE_FAILOVER: return "failover";
    }
    return "unknown";
}
static inline int vwd_mode_from_name(const std::string &n) {
    if (n == "single_best") return VWD_MODE_SINGLE_BEST;
    if (n == "dual_dup") return VWD_MODE_DUAL_DUP;
    if (n == "failover") return VWD_MODE_FAILOVER;
    return -1;
}

struct TermLinkView {
    int id = 0;
    bool alive = false;
    double rtt_ms = -1;
    double loss_pct = -1;
    double bw_kbps = -1;
    uint64_t probes_sent = 0;
    uint64_t probes_acked = 0;
};

struct TerminalStats {
    std::mutex mtx;
    /* 汇聚后的当前快照 */
    std::string mode_name = "";
    bool ebpf_on = false;
    std::string source = ""; /* "wire" | "mqtt" | ""=无数据 */
    std::vector<TermLinkView> links;
    uint64_t dedup_rx = 0, dedup_unique = 0, dedup_dup = 0, dedup_oos = 0, dedup_last_seq = 0;
    uint64_t last_seen_ns = 0;
    uint64_t last_stats_ns = 0; /* 最近一帧 TERM_STATS 到达时刻（云端 steady ns） */
    /* 每帧 TERM_STATS 到达即回调（anchor 线程、持 mtx 中调用——回调内不得再锁 mtx）。
     * 云端借此在同一瞬间采样自己的下行计数器，构成时基一致的匹配对 */
    std::function<void(uint64_t rx, uint64_t unique)> on_stats;

    /* 从 VWD_T_TERM_STATS 包写入（anchor 线程调用） */
    void from_wire(const VwPktHdr &h, const VwTermStatsExtra &ex) {
        std::lock_guard<std::mutex> lk(mtx);
        mode_name = vwd_mode_name((int)ex.mode);
        ebpf_on = ex.ebpf_on != 0;
        source = "wire";
        dedup_rx = ex.dedup_rx;
        dedup_unique = ex.dedup_unique;
        dedup_dup = ex.dedup_dup;
        dedup_oos = ex.dedup_oos;
        dedup_last_seq = ex.dedup_last_seq;
        links.clear();
        for (uint32_t i = 0; i < ex.nlinks && i < 2; ++i) {
            TermLinkView v;
            v.id = (int)ex.links[i].id;
            v.alive = ex.links[i].alive != 0;
            v.rtt_ms = ex.links[i].rtt_us == VWD_TERM_NO_SAMPLE ? -1 : (double)ex.links[i].rtt_us / 1000.0;
            v.loss_pct = ex.links[i].loss_pct_x100 == VWD_TERM_NO_SAMPLE
                             ? -1
                             : (double)ex.links[i].loss_pct_x100 / 100.0;
            v.bw_kbps = ex.links[i].bw_kbps == VWD_TERM_NO_SAMPLE ? -1 : (double)ex.links[i].bw_kbps;
            v.probes_sent = ex.links[i].probes_sent;
            v.probes_acked = ex.links[i].probes_acked;
            links.push_back(v);
        }
        last_seen_ns = h.t_recv_ns ? h.t_recv_ns : now_ns();
        last_stats_ns = now_ns();
        if (on_stats) on_stats(dedup_rx, dedup_unique);
    }

    /* 模式变化检测（供事件记录）：返回变化后的模式名，未变化返回 "" */
    std::string detect_mode_change(const std::string &now_name) {
        std::lock_guard<std::mutex> lk(mtx);
        if (mode_name != now_name && !now_name.empty()) {
            std::string prev = mode_name;
            mode_name = now_name;
            return "mode " + (prev.empty() ? std::string("init") : prev) + " -> " + now_name;
        }
        return "";
    }

    /* 终端 BYE：保留计数/链路快照（验收冻结用），但立即判离线 */
    void mark_offline() {
        std::lock_guard<std::mutex> lk(mtx);
        last_seen_ns = 0;
    }

    bool has_data() const {
        return last_seen_ns != 0 || dedup_rx != 0;
    }
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_TERMINAL_STATS_H
