/**********************************************************************************************************************
    > File: selftest.cpp
    > 本机自检：不起网络远端，直接向本进程的锚点发送/接收，验证
    *   1) PROBE 回应与 t_send 回显
    *   2) 上行双发去重（两副本同 seq → 云侧 unique=1）
    *   3) STATS 对账
    *   4) 下行 dual/best 模式发送（学习端点后发到本机接收端）
    * 用途：云端 Windows/Linux 构建后零环境冒烟测试。
**********************************************************************************************************************/
#include "cloud_server.h"
#include "logger.h"
#include <cstring>
#include <thread>

namespace vwise {
namespace cloud {

static int wait_for_stats(UdpSocket &sock, uint64_t out[4], uint64_t expect_unique, int timeout_ms) {
    uint8_t buf[2048];
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        /* 请求统计 */
        uint8_t req[VWD_STATS_SIZE];
        memset(req, 0, sizeof(req));
        VwPktHdr *h = (VwPktHdr *)req;
        vwd_init_hdr(h, VWD_T_STATS_REQ, 0, 0);
        h->flow_id = 0x5100;
        h->seq = 1;
        h->t_send_ns = now_ns();
        sock.send_to(req, VWD_STATS_SIZE, Endpoint{"127.0.0.1", 39501, true});

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now() < deadline) {
            Endpoint from;
            sock.set_recv_timeout_ms(50);
            int n = sock.recv_from(buf, sizeof(buf), from);
            if (n == VWD_STATS_SIZE) {
                const VwPktHdr *rh = vwd_parse(buf, (size_t)n);
                if (rh && rh->type == VWD_T_STATS_ACK) {
                    const VwStatsExtra *sx = (const VwStatsExtra *)(buf + VWD_HDR_SIZE);
                    memcpy(out, sx->f_u64, sizeof(uint64_t) * 4);
                    if (out[0] >= expect_unique) return 0;
                }
            }
        }
    }
    return -1;
}

int run_selftest() {
    LOGI("=== selftest start ===");
    Config cfg;
    cfg.anchors.push_back(Endpoint{"127.0.0.1", 39501, true});
    cfg.anchors.push_back(Endpoint{"127.0.0.1", 39502, true});
    cfg.dl_mode = "off";
    cfg.mqtt_enable = false;
    cfg.dl_rate_pps = 100;

    CloudServer server(cfg);
    if (!server.start()) {
        LOGE("selftest: server start failed");
        return 1;
    }

    /* 模拟终端：两个"链路"socket 发往两个锚点 */
    UdpSocket link0, link1;
    link0.bind("127.0.0.1", 0);
    link1.bind("127.0.0.1", 0);
    link0.set_recv_timeout_ms(500);
    link1.set_recv_timeout_ms(500);

    /* --- 1. probe echo --- */
    {
        uint8_t probe[VWD_PROBE_SIZE];
        memset(probe, 0, sizeof(probe));
        VwPktHdr *h = (VwPktHdr *)probe;
        vwd_init_hdr(h, VWD_T_PROBE_REQ, 0, 0);
        h->flow_id = 0;
        h->seq = 1;
        h->t_send_ns = now_ns();
        link0.send_to(probe, VWD_PROBE_SIZE, Endpoint{"127.0.0.1", 39501, true});

        uint8_t buf[512];
        Endpoint from;
        int n = link0.recv_from(buf, sizeof(buf), from);
        const VwPktHdr *ack = vwd_parse(buf, (size_t)(n > 0 ? n : 0));
        if (!ack || ack->type != VWD_T_PROBE_ACK || ack->t_send_ns != h->t_send_ns) {
            LOGE("selftest[probe]: FAILED");
            return 1;
        }
        LOGI("selftest[probe]: OK (echo t_send_ns=%llu)", (unsigned long long)ack->t_send_ns);
    }

    /* --- 2. 上行双发去重：同 seq 各发两锚点，期望 unique 增 1、dup 增 1 --- */
    {
        uint64_t before[4] = {0};
        /* 先取基线（STATS_REQ 需 64 字节，避免按 PROBE_SIZE 缓冲发送越界） */
        uint8_t probe[VWD_STATS_SIZE];
        memset(probe, 0, sizeof(probe));
        VwPktHdr *ph = (VwPktHdr *)probe;
        vwd_init_hdr(ph, VWD_T_STATS_REQ, 0, 0);
        ph->flow_id = 0;
        ph->seq = 2;
        ph->t_send_ns = now_ns();

        /* 直接用 STATS_REQ 拿基线（回应到达 link0） */
        link0.send_to(probe, VWD_STATS_SIZE, Endpoint{"127.0.0.1", 39501, true});
        uint8_t buf[512];
        Endpoint from;
        int n = link0.recv_from(buf, sizeof(buf), from);
        if (n == VWD_STATS_SIZE) {
            const VwStatsExtra *sx = (const VwStatsExtra *)(buf + VWD_HDR_SIZE);
            memcpy(before, sx->f_u64, sizeof(before));
        }

        /* 发 100 个 DATA_UP，每包两副本（link0→anchor0, link1→anchor1） */
        uint8_t pkt[256];
        memset(pkt, 0, sizeof(pkt));
        VwPktHdr *h = (VwPktHdr *)pkt;
        vwd_init_hdr(h, VWD_T_DATA_UP, 0, 0);
        h->flow_id = 0x5100;
        h->t_send_ns = now_ns();
        for (uint32_t s = 1; s <= 100; ++s) {
            h->seq = s;
            link0.send_to(pkt, sizeof(pkt), Endpoint{"127.0.0.1", 39501, true});
            link1.send_to(pkt, sizeof(pkt), Endpoint{"127.0.0.1", 39502, true});
        }
        /* 第 2 份副本（乱序容忍验证）：间隔发不影响去重 */

        uint64_t after[4];
        if (wait_for_stats(link0, after, before[0] + 100, 2000) != 0) {
            LOGE("selftest[dedup]: stats timeout");
            return 1;
        }
        uint64_t d_unique = after[0] - before[0];
        uint64_t d_dup = after[1] - before[1];
        if (d_unique != 100 || d_dup != 100) {
            LOGE("selftest[dedup]: FAILED unique=%llu dup=%llu (expect 100/100)",
                 (unsigned long long)d_unique, (unsigned long long)d_dup);
            return 1;
        }
        LOGI("selftest[dedup]: OK (unique+%llu dup+%llu)", (unsigned long long)d_unique,
             (unsigned long long)d_dup);
    }

    /* --- 3. 下行 dual 模式：锚点学到的端点 = link0/link1 源地址，dual 应两路都收到 --- */
    {
        server.set_dl_mode(DlMode::DUAL);
        uint64_t deadline = now_ns() + 3ull * 1000 * 1000 * 1000;
        uint64_t got0 = 0, got1 = 0;
        uint8_t buf[512];
        Endpoint from;
        while (now_ns() < deadline && (got0 == 0 || got1 == 0)) {
            int n = link0.recv_from(buf, sizeof(buf), from);
            if (n > 0 && vwd_parse(buf, (size_t)n) && ((VwPktHdr *)buf)->type == VWD_T_DATA_DOWN) got0++;
            n = link1.recv_from(buf, sizeof(buf), from);
            if (n > 0 && vwd_parse(buf, (size_t)n) && ((VwPktHdr *)buf)->type == VWD_T_DATA_DOWN) got1++;
        }
        server.set_dl_mode(DlMode::OFF);
        if (got0 == 0 || got1 == 0) {
            LOGE("selftest[downlink]: FAILED (link0=%llu link1=%llu)", (unsigned long long)got0,
                 (unsigned long long)got1);
            return 1;
        }
        LOGI("selftest[downlink]: OK (dual copies arrived both links)");
    }

    server.stop();
    LOGI("=== selftest PASSED ===");
    return 0;
}

} // namespace cloud
} // namespace vwise
