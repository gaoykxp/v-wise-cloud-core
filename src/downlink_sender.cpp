/**********************************************************************************************************************
    > File: downlink_sender.cpp
**********************************************************************************************************************/
#include "downlink_sender.h"
#include "logger.h"
#include <chrono>
#include <vector>

namespace vwise {
namespace cloud {

void DownlinkSender::configure(uint32_t flow_id, int payload_bytes, double rate_pps) {
    flow_id_ = flow_id;
    payload_bytes_ = payload_bytes < VWD_HDR_SIZE ? VWD_HDR_SIZE : payload_bytes;
    rate_pps_ = rate_pps > 0.001 ? rate_pps : 50.0;
}

int DownlinkSender::best_anchor_idx() const {
    int best = 0;
    uint64_t best_ts = 0;
    for (size_t i = 0; i < anchors_.size() && i < 8; ++i) {
        uint64_t ts = anchors_[i]->last_rx_ns();
        if (ts > best_ts) { best_ts = ts; best = (int)i; }
    }
    return best;
}

Endpoint DownlinkSender::target_of(int anchor_idx) {
    Endpoint e = anchors_[anchor_idx]->terminal_data_endpoint();
    if (!e.valid) e = anchors_[anchor_idx]->terminal_probe_endpoint();
    return e;
}

bool DownlinkSender::start() {
    if (running_.load()) return true;
    running_.store(true);
    th_ = std::thread(&DownlinkSender::run, this);
    LOGI("downlink: started (flow=0x%08X rate=%.1fpps payload=%dB mode=%d)",
         flow_id_, rate_pps_, payload_bytes_, (int)mode_.load());
    return true;
}

void DownlinkSender::stop() {
    running_.store(false);
    if (th_.joinable()) th_.join();
}

void DownlinkSender::run() {
    std::vector<uint8_t> pkt(payload_bytes_, 0);
    /* duration<double> 不能隐式转 time_point 的整型 duration，须 duration_cast */
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / rate_pps_));
    auto next_t = std::chrono::steady_clock::now();

    while (running_.load()) {
        std::this_thread::sleep_until(next_t);
        next_t += period;

        /* 终端离线 → 暂停发送（seq 不推进，计数不增长），恢复在线自动续发 */
        if (online_check_ && !online_check_()) continue;

        DlMode m = mode_.load();
        if (m == DlMode::OFF) continue;

        uint64_t now = now_ns();
        VwPktHdr *h = (VwPktHdr *)pkt.data();
        vwd_init_hdr(h, VWD_T_DATA_DOWN, 0, 0);
        h->flow_id = flow_id_;
        h->seq = ++seq_;
        h->t_send_ns = now;

        if (m == DlMode::BEST || m == DlMode::AUTO) {
            /* 粘滞选锚：3s 重估一次（锚点活跃度相近时逐包抖动 → 跨 socket 乱序） */
            auto now_tp = std::chrono::steady_clock::now();
            if (cur_best_ < 0 || now_tp - best_eval_at_ > std::chrono::seconds(3)) {
                cur_best_ = best_anchor_idx();
                best_eval_at_ = now_tp;
            }
            int idx = cur_best_;
            Endpoint to = target_of(idx);
            /* 粘滞锚点端点失效（如终端重启未注册）→ 本包退化为任一有效端点 */
            if (!to.valid) {
                for (size_t i = 0; i < anchors_.size() && i < 8; ++i) {
                    to = target_of((int)i);
                    if (to.valid) { idx = (int)i; break; }
                }
            }
            if (to.valid && anchors_[idx]->send_to(pkt.data(), (int)pkt.size(), to)) {
                sent_anchor_[idx & 7].fetch_add(1);
                sent_.fetch_add(1);
            }
        } else { /* DUAL */
            int ok = 0;
            for (size_t i = 0; i < anchors_.size() && i < 8; ++i) {
                Endpoint to = target_of((int)i);
                if (to.valid && anchors_[i]->send_to(pkt.data(), (int)pkt.size(), to)) {
                    sent_anchor_[i & 7].fetch_add(1);
                    ok++;
                }
            }
            if (ok > 0) sent_.fetch_add(1);
        }
    }
}

} // namespace cloud
} // namespace vwise
