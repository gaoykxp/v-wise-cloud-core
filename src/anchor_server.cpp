/**********************************************************************************************************************
    > File: anchor_server.cpp
**********************************************************************************************************************/
#include "anchor_server.h"
#include "logger.h"
#include "terminal_stats.h"
#include <vector>

namespace vwise {
namespace cloud {

bool AnchorServer::start(std::shared_ptr<UplinkStats> stats, std::shared_ptr<TerminalStats> term) {
    stats_ = stats;
    term_ = term;
    if (!sock_.bind(listen_.ip, listen_.port)) {
        LOGE("anchor[%d]: bind %s:%u failed", id_, listen_.ip.c_str(), listen_.port);
        return false;
    }
    if (!sock_.set_recv_timeout_ms(200)) {
        LOGE("anchor[%d]: set timeout failed", id_);
        return false;
    }
    running_.store(true);
    th_ = std::thread(&AnchorServer::run, this);
    LOGI("anchor[%d]: listening on %s:%u", id_, listen_.ip.c_str(), listen_.port);
    return true;
}

void AnchorServer::clear_counters() {
    rx_pkts_.store(0);
    probe_acks_.store(0);
    cur_train_id_ = 0;
    train_bytes_ = 0;
    train_first_ns_ = 0;
    local_dedup_.clear();
}

void AnchorServer::stop() {
    running_.store(false);
    if (th_.joinable()) th_.join();
    sock_.close();
}

Endpoint AnchorServer::terminal_data_endpoint() {
    std::lock_guard<std::mutex> lk(ep_mtx_);
    return has_data_ep_ ? data_ep_ : Endpoint{};
}
Endpoint AnchorServer::terminal_probe_endpoint() {
    std::lock_guard<std::mutex> lk(ep_mtx_);
    return has_probe_ep_ ? probe_ep_ : Endpoint{};
}

void AnchorServer::run() {
    std::vector<uint8_t> buf(2048);
    while (running_.load()) {
        Endpoint from;
        int n = sock_.recv_from(buf.data(), (int)buf.size(), from);
        if (n <= 0) continue; /* 超时轮询 running_ */
        last_rx_.store(now_ns());
        const VwPktHdr *h = vwd_parse(buf.data(), (size_t)n);
        if (!h) continue;
        rx_pkts_.fetch_add(1);

        switch (h->type) {
            case VWD_T_PROBE_REQ:
                handle_probe_req(*h, from, n);
                break;
            case VWD_T_DATA_UP:
                handle_data_up(*h, from);
                break;
            case VWD_T_STATS_REQ:
                handle_stats_req(*h, from);
                break;
            case VWD_T_TERM_STATS:
                handle_term_stats(*h, from, n);
                break;
            default:
                break;
        }
    }
}

void AnchorServer::handle_term_stats(const VwPktHdr &h, const Endpoint &from, int pkt_len) {
    {
        std::lock_guard<std::mutex> lk(ep_mtx_);
        probe_ep_ = from;   /* TERM_STATS 与探测同源 socket */
        has_probe_ep_ = true;
    }
    if (!term_) return;
    if ((size_t)pkt_len < VWD_TERM_STATS_SIZE) return;
    const VwTermStatsExtra *ex = (const VwTermStatsExtra *)((const uint8_t *)&h + VWD_HDR_SIZE);
    term_->from_wire(h, *ex);
    if (h.flags & VWD_F_TERM_BYE) term_->mark_offline(); /* 终端退出：立即冻结（验收窗口闭合） */
}

void AnchorServer::handle_probe_req(const VwPktHdr &h, const Endpoint &from, int pkt_len) {
    {
        std::lock_guard<std::mutex> lk(ep_mtx_);
        probe_ep_ = from;
        has_probe_ep_ = true;
    }

    const VwProbeExtra *ex = nullptr;
    if ((size_t)pkt_len >= VWD_PROBE_SIZE) ex = (const VwProbeExtra *)((const uint8_t *)&h + VWD_HDR_SIZE);

    uint32_t rx_rate = 0, train_bytes = 0;
    if (ex != nullptr && ex->train_id != 0) {
        uint64_t now = now_ns();
        if (ex->train_id != cur_train_id_) {
            /* 新 train */
            cur_train_id_ = ex->train_id;
            train_bytes_ = (uint64_t)pkt_len;
            train_first_ns_ = now;
        } else {
            train_bytes_ += (uint64_t)pkt_len;
        }
        if (h.flags & VWD_F_TRAIN_END) {
            double dt = (double)(now - train_first_ns_);
            if (dt > 1000.0) { /* >1us */
                rx_rate = (uint32_t)((double)train_bytes_ * 8.0 * 1e9 / dt / 1000.0); /* kbps */
            }
            train_bytes = (uint32_t)train_bytes_;
            cur_train_id_ = 0; /* train 结束，等待下一个 */
        }
    }

    /* 回 PROBE_ACK：回显 t_send_ns + 云侧 rx_rate */
    uint8_t out[VWD_PROBE_SIZE];
    memcpy(out, &h, VWD_HDR_SIZE);
    VwPktHdr *rh = (VwPktHdr *)out;
    rh->type = VWD_T_PROBE_ACK;
    rh->t_recv_ns = now_ns();
    VwProbeExtra *rx = (VwProbeExtra *)(out + VWD_HDR_SIZE);
    memset(rx, 0, sizeof(*rx));
    if (ex != nullptr) rx->train_id = ex->train_id;
    rx->rx_rate_kbps = rx_rate;
    rx->train_bytes = train_bytes;
    if (sock_.send_to(out, VWD_PROBE_SIZE, from) > 0) probe_acks_.fetch_add(1);
}

void AnchorServer::handle_data_up(const VwPktHdr &h, const Endpoint &from) {
    {
        std::lock_guard<std::mutex> lk(ep_mtx_);
        data_ep_ = from;
        has_data_ep_ = true;
    }

    uint64_t now = now_ns();
    /* 本锚点独立去重：统计本锚点去重后收到的唯一包 */
    bool local_dup = local_dedup_.seen(h.flow_id, h.seq, now, 60ull * 1000 * 1000 * 1000);

    {
        std::lock_guard<std::mutex> lk(stats_->mtx);
        bool global_dup = stats_->global_dedup.seen(h.flow_id, h.seq, now, 60ull * 1000 * 1000 * 1000);
        if (global_dup) {
            stats_->rx_dup++;
        } else {
            stats_->rx_unique++;
            /* 先到判定：全局唯一且本锚点也首次见 → 本锚点先到 */
            if (!local_dup && id_ < 8) stats_->first_by_anchor[id_]++;
            else if (id_ < 8 && local_dup) { /* 本锚点重复却全局唯一：极小概率乱序回退，忽略 */ }
        }
    }
}

void AnchorServer::handle_stats_req(const VwPktHdr &h, const Endpoint &from) {
    uint8_t out[VWD_STATS_SIZE];
    memcpy(out, &h, VWD_HDR_SIZE);
    VwPktHdr *rh = (VwPktHdr *)out;
    rh->type = VWD_T_STATS_ACK;
    rh->t_recv_ns = now_ns();
    VwStatsExtra *sx = (VwStatsExtra *)(out + VWD_HDR_SIZE);
    memset(sx, 0, sizeof(*sx));
    stats_->snapshot(sx->f_u64);
    sock_.send_to(out, VWD_STATS_SIZE, from);
}

} // namespace cloud
} // namespace vwise
