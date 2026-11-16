/**********************************************************************************************************************
    > File: cloud_server.cpp
**********************************************************************************************************************/
#include "cloud_server.h"
#include "logger.h"
#include <cstdarg>
#include <cstring>

namespace vwise {
namespace cloud {

CloudServer::CloudServer(const Config &cfg) : cfg_(cfg), term_(std::make_shared<TerminalStats>()) {
    stats_ = std::make_shared<UplinkStats>();
    for (size_t i = 0; i < cfg_.anchors.size(); ++i) {
        anchors_.push_back(std::make_unique<AnchorServer>((int)i, cfg_.anchors[i]));
    }
    for (auto &a : anchors_) anchor_ptrs_.push_back(a.get());

    dl_ = std::make_unique<DownlinkSender>(anchor_ptrs_);
    dl_->configure(cfg_.dl_flow_id, cfg_.dl_payload_bytes, cfg_.dl_rate_pps);
    /* 匹配对采样：每帧 TERM_STATS 到达瞬间同步读云端下行计数（时基一致） */
    term_->on_stats = [this](uint64_t rx, uint64_t unique) {
        uint64_t copies = 0;
        for (int i = 0; i < 8 && i < (int)anchor_ptrs_.size(); ++i)
            copies += dl_->sent_by_anchor(i);
        std::lock_guard<std::mutex> lk(match_mtx_);
        match_term_rx_ = rx;
        match_term_unique_ = unique;
        match_dl_copies_ = copies;
        match_dl_sent_ = dl_->sent_total();
    };
    /* 终端在线门控：TERM_STATS 4s 新鲜 OR 任一锚点 4s 内有终端来包。
     * 后者兜底两类场景：selftest（无 TERM_STATS）、纯探测/下行注册（DATA_UP 活性）。
     * 终端退出后两种信号同时停更 → 门控关闭，验收分母不再被"死发包"污染 */
    dl_->set_online_check([this]() {
        {
            std::lock_guard<std::mutex> lk(term_->mtx);
            if (term_->last_seen_ns != 0 &&
                now_ns() - term_->last_seen_ns < 4ull * 1000 * 1000 * 1000)
                return true;
        }
        for (auto &a : anchor_ptrs_) {
            if (a->last_rx_ns() != 0 &&
                now_ns() - a->last_rx_ns() < 4ull * 1000 * 1000 * 1000)
                return true;
        }
        return false;
    });
}

bool CloudServer::start() {
    for (auto &a : anchors_) {
        if (!a->start(stats_, term_)) return false;
    }

    DlMode m = DlMode::DUAL;
    if (cfg_.dl_mode == "off") m = DlMode::OFF;
    else if (cfg_.dl_mode == "best") m = DlMode::BEST;
    else if (cfg_.dl_mode == "auto") m = DlMode::AUTO;
    dl_->set_mode(m);
    dl_->start();

    if (cfg_.mqtt_enable) {
        mqtt_ = std::make_unique<MiniMqtt>(cfg_.mqtt_host, cfg_.mqtt_port, cfg_.mqtt_client_id);
        mqtt_->add_subscribe(cfg_.mqtt_status_filter);
        mqtt_->set_on_message([this](const std::string &t, const std::string &p) {
            on_mqtt_message(t, p);
        });
        if (!mqtt_->start()) {
            LOGW("mqtt disabled (broker unreachable) — 数据面不受影响");
            mqtt_.reset();
        }
    }

    reset_epoch_locked();
    running_.store(true);
    status_th_ = std::thread(&CloudServer::status_loop, this);
    add_event("server", "started");
    LOGI("cloud server: %zu anchors, downlink=%s rate=%.1fpps", anchors_.size(),
         dl_mode_name(dl_->mode()), cfg_.dl_rate_pps);
    return true;
}

void CloudServer::stop() {
    bool was_running = running_.exchange(false);
    if (status_th_.joinable()) status_th_.join();
    if (dl_) dl_->stop();
    for (auto &a : anchors_) a->stop();
    if (mqtt_) mqtt_->stop();
    if (was_running) add_event("server", "stopped");
}

void CloudServer::set_dl_mode(DlMode m) {
    add_event("control", std::string("dl_mode -> ") + dl_mode_name(m));
    dl_->set_mode(m);
    reset_epoch_locked();
    LOGI("downlink mode -> %s", dl_mode_name(m));
}

void CloudServer::clear_stats() {
    stats_->clear_counters();
    for (auto &a : anchors_) a->clear_counters();
    {
        std::lock_guard<std::mutex> lk(term_->mtx);
        term_->dedup_rx = term_->dedup_unique = term_->dedup_dup = term_->dedup_oos = 0;
        term_->dedup_last_seq = 0;
    }
    last_unique_ = last_dup_ = last_dl_sent_ = 0;
    last_first_[0] = last_first_[1] = 0;
    reset_epoch_locked();
    add_event("control", "stats cleared");
}

bool CloudServer::send_ctrl(const std::string &json) {
    if (json.empty() || json.size() > VWD_CTRL_MAX) return false;
    uint8_t pkt[VWD_HDR_SIZE + VWD_CTRL_MAX];
    memset(pkt, 0, sizeof(pkt));
    VwPktHdr *h = (VwPktHdr *)pkt;
    vwd_init_hdr(h, VWD_T_CTRL, 0, 0);
    h->t_send_ns = now_ns();
    memcpy(pkt + VWD_HDR_SIZE, json.c_str(), json.size());

    /* 发往终端探测端点（TERM_STATS/PROBE 源地址），优先最近活跃锚点 */
    int order[2] = {0, 1};
    if (anchors_.size() > 1 && anchor_ptrs_[1]->last_rx_ns() > anchor_ptrs_[0]->last_rx_ns()) {
        order[0] = 1;
        order[1] = 0;
    }
    for (int oi = 0; oi < (int)anchors_.size() && oi < 2; ++oi) {
        AnchorServer *a = anchor_ptrs_[order[oi]];
        Endpoint to = a->terminal_probe_endpoint();
        if (!to.valid) to = a->terminal_data_endpoint();
        if (!to.valid) continue;
        if (a->send_to(pkt, (int)(VWD_HDR_SIZE + json.size()), to)) {
            add_event("control", "ctrl sent: " + json);
            return true;
        }
    }
    return false;
}

void CloudServer::add_event(const std::string &type, const std::string &detail) {
    std::lock_guard<std::mutex> lk(ev_mtx_);
    WebEvent e;
    e.ts_ms = now_ms_epoch();
    e.type = type;
    e.detail = detail;
    events_.push_back(std::move(e));
    if (events_.size() > 500) events_.pop_front();
}

void CloudServer::reset_epoch_locked() {
    epoch_reset_ns_ = now_ns();
    epoch_term_pending_ = true; /* 终端基线延迟到下一帧新鲜 TERM_STATS 再取 */
    uint64_t s[4];
    stats_->snapshot(s);
    epoch_unique_ = s[0];
    epoch_dup_ = s[1];
    epoch_first_[0] = s[2];
    epoch_first_[1] = s[3];
    {
        std::lock_guard<std::mutex> lk(match_mtx_);
        epoch_match_dl_copies_ = match_dl_copies_;
        epoch_match_dl_sent_ = match_dl_sent_;
    }
    {
        std::lock_guard<std::mutex> lk(term_->mtx);
        epoch_term_rx_ = term_->dedup_rx;
        epoch_term_unique_ = term_->dedup_unique;
        epoch_term_mode_ = term_->mode_name;
    }
    epoch_dl_mode_ = dl_mode_name(dl_->mode());
}

void CloudServer::on_mqtt_message(const std::string &topic, const std::string &payload) {
    bool ok = false;
    Json j = Json::parse(payload, &ok);
    if (!ok || j.type != Json::OBJ) {
        LOGW("mqtt: bad status json on %s", topic.c_str());
        return;
    }
    term_imei_ = j.get_str("imei", "");
    std::string mode = j.get_str("mode", "");
    std::string ebpf = j.get_str("ebpf", "");

    std::lock_guard<std::mutex> lk(term_->mtx);
    term_->source = "mqtt";
    if (!mode.empty()) term_->mode_name = mode;
    term_->ebpf_on = (ebpf == "on");
    term_->last_seen_ns = now_ns();
    auto it = j.obj.find("links");
    if (it != j.obj.end() && it->second.type == Json::ARR) {
        term_->links.clear();
        for (auto &l : it->second.arr) {
            if (l.type != Json::OBJ) continue;
            TermLinkView v;
            v.id = (int)l.get_num("id", 0);
            v.alive = l.get_num("alive", 0) != 0;
            v.rtt_ms = l.get_num("rtt_ms", -1);
            v.loss_pct = l.get_num("loss_pct", -1);
            v.bw_kbps = l.get_num("bw_kbps", -1);
            term_->links.push_back(v);
        }
    }
    LOGI("mqtt: status from imei=%s mode=%s links=%zu", term_imei_.c_str(),
         term_->mode_name.c_str(), term_->links.size());
}

void CloudServer::decide_auto_mode() {
    /* auto 规则（P2 初版）：终端两条链路均 alive 且丢包 < 5% → dual；否则 best */
    std::vector<TermLinkView> links;
    term_->mtx.lock();
    links = term_->links;
    bool has = term_->has_data();
    term_->mtx.unlock();
    if (!has || links.empty()) return;
    bool all_alive = true;
    for (auto &l : links) {
        if (!l.alive || (l.loss_pct >= 0 && l.loss_pct >= 5.0)) all_alive = false;
    }
    DlMode want = all_alive ? DlMode::DUAL : DlMode::BEST;
    if (dl_->mode() != want) {
        add_event("control", std::string("auto mode -> ") + dl_mode_name(want));
        dl_->set_mode(want);
        reset_epoch_locked();
        LOGI("auto mode -> %s", dl_mode_name(want));
    }
}

void CloudServer::status_loop() {
    double interval = cfg_.stats_interval_sec > 0.1 ? cfg_.stats_interval_sec : 2.0;
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(interval));
    auto next_t = std::chrono::steady_clock::now();

    while (running_.load()) {
        next_t += period;
        std::this_thread::sleep_until(next_t);

        if (dl_->mode() == DlMode::AUTO) decide_auto_mode();

        /* ---- 终端状态变化事件（模式 / 链路 alive 边沿 / 上线沿·离线沿）---- */
        {
            std::lock_guard<std::mutex> lk(term_->mtx);
            bool online = term_->last_seen_ns != 0 &&
                          (now_ns() - term_->last_seen_ns < 15ull * 1000 * 1000 * 1000);
            if (online && !last_term_online_) {
                /* 终端上线沿：验收 epoch 重置——避免"终端离线期间云端已发"的包污染分母 */
                add_event("server", "terminal online -> acceptance epoch reset");
                stats_->epoch_dirty.store(true);
            }
            if (!online && last_term_online_) {
                /* 终端离线沿（BYE/超时）：匹配对不再更新，验收窗口自然冻结在离线时刻 */
                add_event("server", "terminal offline -> acceptance frozen");
            }
            last_term_online_ = online;

            if (term_->has_data()) {
                if (!term_->mode_name.empty() && term_->mode_name != last_term_mode_seen_) {
                    std::string prev = last_term_mode_seen_;
                    add_event("mode_change",
                              prev.empty() ? ("terminal mode -> " + term_->mode_name)
                                           : ("terminal mode " + prev + " -> " + term_->mode_name));
                    last_term_mode_seen_ = term_->mode_name;
                    stats_->epoch_dirty.store(true); /* 模式变化 → 验收 epoch 重置 */
                }
                for (size_t i = 0; i < term_->links.size() && i < 2; ++i) {
                    std::string cur = term_->links[i].alive ? "true" : "false";
                    std::string &last = last_term_link_alive_[i];
                    if (!last.empty() && last != cur) {
                        add_event(cur == "false" ? "link_down" : "link_up",
                                  "terminal link " + std::to_string(i));
                    }
                    last = cur;
                }
            }
        }
        if (stats_->epoch_dirty.exchange(false)) reset_epoch_locked();
        if (epoch_term_pending_) {
            /* 重置后第一帧新鲜 TERM_STATS：以它为终端基线（模式标签同步刷新）；
             * 该帧的匹配对同帧对齐，一并取下行侧基线 */
            std::lock_guard<std::mutex> lk(term_->mtx);
            if (term_->last_stats_ns >= epoch_reset_ns_) {
                epoch_term_rx_ = term_->dedup_rx;
                epoch_term_unique_ = term_->dedup_unique;
                epoch_term_mode_ = term_->mode_name;
                std::lock_guard<std::mutex> lk2(match_mtx_);
                epoch_match_dl_copies_ = match_dl_copies_;
                epoch_match_dl_sent_ = match_dl_sent_;
                epoch_term_pending_ = false;
            }
        }

        /* ---- 周期增量 → 时序采样 ---- */
        uint64_t s[4];
        stats_->snapshot(s);
        uint64_t d_unique = s[0] - last_unique_;
        uint64_t d_dup = s[1] - last_dup_;
        uint64_t d_first0 = s[2] - last_first_[0];
        uint64_t d_first1 = s[3] - last_first_[1];
        uint64_t dl_sent = dl_->sent_total();
        uint64_t d_dl = dl_sent - last_dl_sent_;
        last_unique_ = s[0];
        last_dup_ = s[1];
        last_first_[0] = s[2];
        last_first_[1] = s[3];
        last_dl_sent_ = dl_sent;

        HistSample hs;
        hs.t_ms = now_ms_epoch();
        double d_total = (double)d_unique + (double)d_dup;
        hs.dup_ratio = d_total > 0 ? 100.0 * (double)d_dup / d_total : 0.0;
        hs.thr_up0 = (double)d_first0 / interval;
        hs.thr_up1 = (double)d_first1 / interval;
        hs.dl_pps = (double)d_dl / interval;
        {
            std::lock_guard<std::mutex> lk(term_->mtx);
            for (size_t i = 0; i < term_->links.size() && i < 2; ++i)
                hs.rtt_ms[i] = term_->links[i].rtt_ms;
            hs.term_dup_ratio = term_->dedup_rx > 0
                                    ? 100.0 * (double)(term_->dedup_rx - term_->dedup_unique) /
                                          (double)term_->dedup_rx
                                    : -1;
            last_term_rx_ = term_->dedup_rx;
        }
        {
            std::lock_guard<std::mutex> lk(hist_mtx_);
            history_.push_back(hs);
            if (history_.size() > 600) history_.pop_front();
        }

        /* ---- 控制台统计行（与看板同源）---- */
        LOGI("[stats] up: unique=%llu dup=%llu(%+.0f) first0=%llu(+%llu) first1=%llu(+%llu) "
             "| dl: %s %llu(+%llu) | term: %s rx=%llu",
             (unsigned long long)s[0], (unsigned long long)s[1],
             (double)d_dup, (unsigned long long)s[2], (unsigned long long)d_first0,
             (unsigned long long)s[3], (unsigned long long)d_first1,
             dl_mode_name(dl_->mode()), (unsigned long long)dl_sent,
             (unsigned long long)d_dl,
             last_term_mode_seen_.c_str(), (unsigned long long)last_term_rx_);
    }
}

/* ==================== 看板 JSON 出口 ==================== */

static std::string jfmt(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string CloudServer::build_overview_json() {
    uint64_t s[4];
    stats_->snapshot(s);
    std::string j = "{";
    j += jfmt("\"ts\":%llu,", (unsigned long long)now_ms_epoch());

    /* 上/下行模式 */
    j += jfmt("\"dl_mode\":\"%s\",", dl_mode_name(dl_->mode()));
    {
        std::lock_guard<std::mutex> lk(term_->mtx);
        j += jfmt("\"term_mode\":\"%s\",\"term_ebpf\":\"%s\",\"term_source\":\"%s\",",
                  term_->mode_name.c_str(), term_->ebpf_on ? "on" : "fallback",
                  term_->source.c_str());
        j += jfmt("\"term_alive\":%s,",
                  term_->has_data() && (now_ns() - term_->last_seen_ns < 15ull * 1000 * 1000 * 1000)
                      ? "true"
                      : "false");

        /* 终端链路 */
        j += "\"term_links\":[";
        for (size_t i = 0; i < term_->links.size(); ++i) {
            j += jfmt("{\"id\":%d,\"alive\":%s,\"rtt_ms\":%.1f,\"loss_pct\":%.2f,"
                      "\"bw_kbps\":%.0f,\"sent\":%llu,\"acked\":%llu}%s",
                      term_->links[i].id, term_->links[i].alive ? "true" : "false",
                      term_->links[i].rtt_ms, term_->links[i].loss_pct, term_->links[i].bw_kbps,
                      (unsigned long long)term_->links[i].probes_sent,
                      (unsigned long long)term_->links[i].probes_acked,
                      i + 1 < term_->links.size() ? "," : "");
        }
        j += "],";
        j += jfmt("\"term_dedup\":{\"rx\":%llu,\"unique\":%llu,\"dup\":%llu,\"oos\":%llu,"
                  "\"last_seq\":%llu},",
                  (unsigned long long)term_->dedup_rx, (unsigned long long)term_->dedup_unique,
                  (unsigned long long)term_->dedup_dup, (unsigned long long)term_->dedup_oos,
                  (unsigned long long)term_->dedup_last_seq);
    }

    /* 上行统计 */
    j += jfmt("\"uplink\":{\"unique\":%llu,\"dup\":%llu,\"first\":[%llu,%llu],"
              "\"anchor_rx\":[",
              (unsigned long long)s[0], (unsigned long long)s[1],
              (unsigned long long)s[2], (unsigned long long)s[3]);
    for (size_t i = 0; i < anchors_.size() && i < 8; ++i) {
        j += jfmt("%s%llu", i ? "," : "", (unsigned long long)anchors_[i]->rx_packets());
    }
    j += "],\"probe_ack\":[";
    for (size_t i = 0; i < anchors_.size() && i < 8; ++i) {
        j += jfmt("%s%llu", i ? "," : "", (unsigned long long)anchors_[i]->probe_acks());
    }
    j += "]},";

    /* 下行统计 */
    j += jfmt("\"downlink\":{\"sent\":%llu,\"by_anchor\":[",
              (unsigned long long)dl_->sent_total());
    for (int i = 0; i < (int)anchors_.size() && i < 8; ++i) {
        j += jfmt("%s%llu", i ? "," : "", (unsigned long long)dl_->sent_by_anchor(i));
    }
    j += "]},";

    /* 验收判定 */
    j += "\"acceptance\":[";
    append_acceptance(j);
    j += "]}";
    return j;
}

void CloudServer::append_acceptance(std::string &j) {
    uint64_t s[4];
    stats_->snapshot(s);
    /* P2 项全部使用匹配对：终端计数与云端下行计数都在最近一帧 TERM_STATS
     * 到达瞬间采样，两源时基一致——无快照滞后偏差；终端静默后匹配对自然冻结 */
    uint64_t m_rx, m_uniq, m_copies, m_sent;
    {
        std::lock_guard<std::mutex> lk(match_mtx_);
        m_rx = match_term_rx_;
        m_uniq = match_term_unique_;
        m_copies = match_dl_copies_;
        m_sent = match_dl_sent_;
    }
    uint64_t e_unique = s[0] - epoch_unique_;
    uint64_t e_dup = s[1] - epoch_dup_;
    uint64_t e_first0 = s[2] - epoch_first_[0];
    uint64_t e_first1 = s[3] - epoch_first_[1];
    uint64_t e_dl = m_sent - epoch_match_dl_sent_;
    /* DUAL 下若副锚点尚未学到终端端点会少发副本——分母用匹配对中的实际副本数 */
    uint64_t e_dl_copies = m_copies - epoch_match_dl_copies_;
    uint64_t e_term_rx = m_rx - epoch_term_rx_;
    uint64_t e_term_unique = m_uniq - epoch_term_unique_;

    const int kMinPkts = 100; /* 样本下限，不足判 n/a */
    auto entry = [&j](const char *id, const char *desc, double value, int status) {
        static const char *st[] = {"na", "pass", "fail"};
        j += jfmt("{\"id\":\"%s\",\"desc\":\"%s\",\"value\":%.3f,\"status\":\"%s\"},",
                  id, desc, value, st[status < 0 || status > 2 ? 0 : status]);
    };

    /* P1-1: dual_dup 期间 dup/unique ≈ 1（模式标签取自 TERM_STATS，基线未定前不评估） */
    if (!epoch_term_pending_ && epoch_term_mode_ == "dual_dup" && e_unique >= kMinPkts) {
        double v = e_unique > 0 ? (double)e_dup / (double)e_unique : 0;
        entry("P1-1", "dup/unique (dual)", v, (v > 0.98 && v < 1.02) ? 1 : 2);
    } else entry("P1-1", "dup/unique (dual)", -1, 0);

    /* P1-2: (first0+first1)/unique ≈ 1 */
    if (e_unique >= kMinPkts) {
        double v = (double)(e_first0 + e_first1) / (double)e_unique;
        entry("P1-2", "(first0+first1)/unique", v, (v > 0.98 && v < 1.02) ? 1 : 2);
    } else entry("P1-2", "(first0+first1)/unique", -1, 0);

    /* P1-3: 防复制风暴——任一锚点收包/unique <= 2.2（双发每锚点至多 1×unique + 探测） */
    if (e_unique >= kMinPkts && anchor_ptrs_.size() >= 2) {
        double v = (double)anchors_[0]->rx_packets() / (double)(s[0] > 0 ? s[0] : 1);
        entry("P1-3", "anchor_rx/unique (storm)", v, v < 2.2 ? 1 : 2);
    } else entry("P1-3", "anchor_rx/unique (storm)", -1, 0);

    /* P2-1: dual 下行 终端 rx / 实际发出副本数 ≈ 1
     * 容差 ±5%：TERM_STATS 2s 快照与连续计数器存在固有采样时基偏差 */
    if (!epoch_term_pending_ && epoch_dl_mode_ == "dual" && e_dl >= kMinPkts && e_dl_copies > 0 &&
        e_term_rx > 0) {
        double v = (double)e_term_rx / (double)e_dl_copies;
        entry("P2-1", "term_rx/dl_copies", v, (v > 0.95 && v < 1.05) ? 1 : 2);
    } else entry("P2-1", "term_rx/dl_copies", -1, 0);

    /* P2-2: best 下行 终端重复占比 < 1% */
    if (!epoch_term_pending_ && epoch_dl_mode_ == "best" && e_term_rx >= kMinPkts) {
        double v = e_term_rx > e_term_unique ? (double)(e_term_rx - e_term_unique) / (double)e_term_rx : 0;
        entry("P2-2", "term_dup_ratio (best)", v, v < 0.01 ? 1 : 2);
    } else entry("P2-2", "term_dup_ratio (best)", -1, 0);

    /* P2-3: 时延差注入下 终端 unique/dl_sent ≥ 0.95（同上采样容差） */
    if (!epoch_term_pending_ && e_dl >= kMinPkts && e_term_unique > 0) {
        double v = (double)e_term_unique / (double)e_dl;
        entry("P2-3", "term_unique/dl_sent", v, v >= 0.95 ? 1 : 2);
    } else entry("P2-3", "term_unique/dl_sent", -1, 0);

    /* 去掉末尾多余逗号 */
    if (!j.empty() && j.back() == ',') j.pop_back();
}

std::string CloudServer::build_events_json(int n) {
    std::lock_guard<std::mutex> lk(ev_mtx_);
    std::string j = "{\"events\":[";
    int count = 0;
    int total = (int)events_.size();
    for (int idx = total - 1; idx >= 0 && count < n; --idx, ++count) {
        auto &e = events_[idx];
        j += jfmt("{\"ts\":%llu,\"type\":\"%s\",\"detail\":\"%s\"}%s",
                  (unsigned long long)e.ts_ms, e.type.c_str(), e.detail.c_str(),
                  count + 1 < n && idx - 1 >= 0 ? "," : "");
    }
    if (!j.empty() && j.back() == ',') j.pop_back();
    j += "]}";
    return j;
}

std::string CloudServer::build_history_json() {
    std::lock_guard<std::mutex> lk(hist_mtx_);
    std::string j = "{\"samples\":[";
    bool first = true;
    for (auto &hs : history_) {
        j += jfmt("%s[%llu,%.2f,%.1f,%.1f,%.2f,%.2f,%.1f,%.2f]",
                  first ? "" : ",", (unsigned long long)hs.t_ms, hs.dup_ratio,
                  hs.rtt_ms[0], hs.rtt_ms[1], hs.thr_up0, hs.thr_up1, hs.dl_pps,
                  hs.term_dup_ratio);
        first = false;
    }
    j += "]}";
    return j;
}

std::string CloudServer::build_prometheus_text() {
    uint64_t s[4];
    stats_->snapshot(s);
    std::string j;
    j += jfmt("# HELP vwise_uplink_unique Unique uplink packets after dedup\n"
              "# TYPE vwise_uplink_unique counter\nvwise_uplink_unique %llu\n",
              (unsigned long long)s[0]);
    j += jfmt("# TYPE vwise_uplink_dup counter\nvwise_uplink_dup %llu\n",
              (unsigned long long)s[1]);
    j += jfmt("# TYPE vwise_first_by_anchor counter\nvwise_first_by_anchor{anchor=\"0\"} %llu\n"
              "vwise_first_by_anchor{anchor=\"1\"} %llu\n",
              (unsigned long long)s[2], (unsigned long long)s[3]);
    j += jfmt("# TYPE vwise_dl_sent counter\nvwise_dl_sent %llu\n",
              (unsigned long long)dl_->sent_total());
    return j;
}

bool CloudServer::handle_ctl(const std::string &body, std::string &err) {
    bool ok = false;
    Json j = Json::parse(body, &ok);
    if (!ok || j.type != Json::OBJ) {
        err = "bad json";
        return false;
    }
    std::string action = j.get_str("action", "");
    std::string mode = j.get_str("mode", "");
    if (action == "dl_mode") {
        if (mode == "off") set_dl_mode(DlMode::OFF);
        else if (mode == "best") set_dl_mode(DlMode::BEST);
        else if (mode == "dual") set_dl_mode(DlMode::DUAL);
        else if (mode == "auto") set_dl_mode(DlMode::AUTO);
        else { err = "bad mode"; return false; }
        return true;
    }
    if (action == "clear_stats") {
        clear_stats();
        return true;
    }
    if (action == "term_mode") {
        /* 终端模式指令：走 CTRL 包（无 broker）与 MQTT（有 broker 且已学到 imei）双发 */
        std::string payload = mode.empty() ? body : "{\"mode\":\"" + mode + "\"}";
        bool sent = send_ctrl(payload);
        if (mqtt_ && !term_imei_.empty()) {
            std::string topic = cfg_.mqtt_mode_prefix + term_imei_ + "/link/mode/down";
            mqtt_->publish(topic, payload);
        }
        if (!sent && !(mqtt_ && !term_imei_.empty())) {
            err = "terminal endpoint unknown (waiting for TERM_STATS/probe)";
            return false;
        }
        return true;
    }
    if (action == "train") {
        bool sent = send_ctrl("{\"action\":\"train\"}");
        if (!sent) { err = "terminal endpoint unknown"; return false; }
        return true;
    }
    err = "unknown action";
    return false;
}

} // namespace cloud
} // namespace vwise
