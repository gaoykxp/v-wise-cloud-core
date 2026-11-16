/**********************************************************************************************************************
    > File: web_dashboard.cpp
    * 极简 HTTP/1.1 服务（GET/POST，Connection: close），路由白名单，无路径穿越。
**********************************************************************************************************************/
#include "web_dashboard.h"
#include "logger.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace vwise {
namespace cloud {

bool WebDashboard::start(uint16_t port) {
    if (running_.exchange(true)) return true;
    if (!listen_.listen(port)) {
        running_.store(false);
        LOGE("web: listen %u failed", port);
        return false;
    }
    th_ = std::thread(&WebDashboard::accept_loop, this);
    LOGI("web dashboard: http://0.0.0.0:%u/ (www=%s)", port, www_dir_.c_str());
    return true;
}

void WebDashboard::stop() {
    if (!running_.exchange(false)) return;
    listen_.close(); /* 使阻塞 accept 退出 */
    if (th_.joinable()) th_.join();
}

void WebDashboard::accept_loop() {
    while (running_.load()) {
        TcpSocket *conn = new TcpSocket();
        if (!listen_.accept(*conn)) {
            delete conn;
            if (!running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        std::thread t(&WebDashboard::handle_conn, this, conn);
        t.detach();
    }
}

static void http_send(TcpSocket &s, int status, const std::string &ctype,
                      const std::string &body) {
    static const char *reason[] = {"OK", "Bad Request", "Not Found", "Server Error"};
    const char *r = status == 200 ? reason[0]
                  : status == 400 ? reason[1]
                  : status == 404 ? reason[2] : reason[3];
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
             status, r, ctype.c_str(), body.size());
    s.send_all(head, (int)strlen(head));
    s.send_all(body.data(), (int)body.size());
}

void WebDashboard::handle_conn(TcpSocket *conn) {
    std::unique_ptr<TcpSocket> guard(conn);
    conn->set_recv_timeout_ms(3000);

    /* 读到头部结束（最多 8KB） */
    std::string req;
    char buf[2048];
    size_t header_end = std::string::npos;
    while (req.size() < 8192) {
        int n = conn->recv(buf, sizeof(buf));
        if (n <= 0) break;
        req.append(buf, (size_t)n);
        header_end = req.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
    }
    if (header_end == std::string::npos) {
        http_send(*conn, 400, "text/plain", "bad request");
        return;
    }

    /* 解析请求行：METHOD PATH HTTP/x */
    size_t line_end = req.find("\r\n");
    std::string line = req.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) {
        http_send(*conn, 400, "text/plain", "bad request");
        return;
    }
    std::string method = line.substr(0, sp1);
    std::string full_path = line.substr(sp1 + 1, sp2 - sp1 - 1);

    /* 拆 query */
    std::string path = full_path, query;
    size_t q = full_path.find('?');
    if (q != std::string::npos) {
        path = full_path.substr(0, q);
        query = full_path.substr(q + 1);
    }

    /* POST body：Content-Length */
    std::string body;
    if (method == "POST") {
        size_t cl_pos = req.find("Content-Length:");
        if (cl_pos == std::string::npos) cl_pos = req.find("content-length:");
        size_t body_start = header_end + 4;
        size_t body_have = req.size() - body_start;
        size_t want = 0;
        if (cl_pos != std::string::npos) want = (size_t)atoi(req.c_str() + cl_pos + 15);
        if (want > 4096) want = 4096;
        while (body_have < want) {
            int n = conn->recv(buf, sizeof(buf));
            if (n <= 0) break;
            req.append(buf, (size_t)n);
            body_have = req.size() - body_start;
        }
        if (body_start < req.size()) body = req.substr(body_start, want);
    }

    std::string out, ctype = "application/json";
    int status = 200;
    if (!route(method, path, query, body, out, ctype, status)) status = 404;
    http_send(*conn, status, ctype, out);
}

bool WebDashboard::serve_file(const std::string &rel, std::string &out, std::string &ctype) {
    /* 白名单化文件名，杜绝路径穿越 */
    static const std::map<std::string, std::string> kFiles = {
        {"index.html", "text/html; charset=utf-8"},
        {"app.js", "text/javascript; charset=utf-8"},
        {"style.css", "text/css; charset=utf-8"},
    };
    auto it = kFiles.find(rel);
    if (it == kFiles.end()) return false;
    std::ifstream f(www_dir_ + "/" + it->first, std::ios::binary);
    if (!f.is_open()) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ctype = it->second;
    return true;
}

bool WebDashboard::route(const std::string &method, const std::string &path,
                         const std::string &query, const std::string &body,
                         std::string &out, std::string &ctype, int &status) {
    (void)status;
    if (method == "GET") {
        if (path == "/" || path == "/index.html") {
            if (serve_file("index.html", out, ctype)) return true;
            out = "<html><body>vwise-cloud-core<br>www/ index.html not found</body></html>";
            ctype = "text/html";
            return true;
        }
        if (path == "/app.js" || path == "/style.css") {
            return serve_file(path.substr(1), out, ctype);
        }
        if (path == "/api/overview") {
            out = srv_.build_overview_json();
            return true;
        }
        if (path == "/api/events") {
            int n = 100;
            size_t p = query.find("n=");
            if (p != std::string::npos) n = atoi(query.c_str() + p + 2);
            if (n <= 0 || n > 500) n = 100;
            out = srv_.build_events_json(n);
            return true;
        }
        if (path == "/api/history") {
            out = srv_.build_history_json();
            return true;
        }
        if (path == "/metrics") {
            out = srv_.build_prometheus_text();
            ctype = "text/plain; version=0.0.4; charset=utf-8";
            return true;
        }
        return false;
    }
    if (method == "POST" && path == "/api/ctl") {
        std::string err;
        if (srv_.handle_ctl(body, err)) {
            out = "{\"ok\":true}";
        } else {
            out = "{\"ok\":false,\"error\":\"" + err + "\"}";
        }
        return true;
    }
    return false;
}

} // namespace cloud
} // namespace vwise
