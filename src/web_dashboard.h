/**********************************************************************************************************************
    > File: web_dashboard.h
    > 内嵌 Web 看板（测试联调用，局域网专用，无鉴权）：
    *   GET  /                静态 index.html（www/ 目录）
    *   GET  /app.js|style.css 静态资源
    *   GET  /api/overview     全量快照 JSON（卡片+验收判定）
    *   GET  /api/events?n=100 事件日志
    *   GET  /api/history      时序（10min 环形缓冲）
    *   GET  /metrics          Prometheus 文本（生产出口预留）
    *   POST /api/ctl          控制动作 {"action":..., "mode":...}
    * 线程模型：accept 线程 + 每连接一短线程（Connection: close，1s 轮询场景足够）。
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_WEB_DASHBOARD_H
#define VWISE_CLOUD_WEB_DASHBOARD_H

#include "cloud_server.h"
#include "net.h"
#include <atomic>
#include <string>
#include <thread>

namespace vwise {
namespace cloud {

class WebDashboard {
public:
    WebDashboard(CloudServer &srv, std::string www_dir)
        : srv_(srv), www_dir_(std::move(www_dir)) {}
    ~WebDashboard() { stop(); }

    bool start(uint16_t port);
    void stop();

private:
    void accept_loop();
    void handle_conn(TcpSocket *conn);
    /* 返回 false=未命中路由；命中时填 body/ctype */
    bool route(const std::string &method, const std::string &path,
               const std::string &query, const std::string &body,
               std::string &out, std::string &ctype, int &status);
    bool serve_file(const std::string &rel, std::string &out, std::string &ctype);

    CloudServer &srv_;
    TcpServerSocket listen_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::string www_dir_;
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_WEB_DASHBOARD_H
