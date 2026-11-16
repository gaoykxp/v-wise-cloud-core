/**********************************************************************************************************************
    > File: main.cpp
    > vwise-cloud-core 入口：云端双链路远控网关（P0-P2 联调版）
    > 用法：
    *   vwise_cloud_core --config cloud.ini
    *   vwise_cloud_core --selftest
    *   vwise_cloud_core --config cloud.ini --dl-mode dual|best|off|auto --no-mqtt
**********************************************************************************************************************/
#include "cloud_server.h"
#include "logger.h"
#include "web_dashboard.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace vwise {
namespace cloud {

int run_selftest();

} // namespace cloud
} // namespace vwise

static std::atomic<bool> g_run{true};
static void on_sig(int) { g_run.store(false); }

int main(int argc, char **argv) {
    using namespace vwise::cloud;

    std::string cfg_path = "cloud.ini";
    bool selftest = false, no_mqtt = false, no_web = false;
    std::string dl_mode_override;
    int log_level = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--selftest") selftest = true;
        else if (a == "--no-mqtt") no_mqtt = true;
        else if (a == "--no-web") no_web = true;
        else if (a == "--config" && i + 1 < argc) cfg_path = argv[++i];
        else if (a == "--dl-mode" && i + 1 < argc) dl_mode_override = argv[++i];
        else if (a == "--log-level" && i + 1 < argc) log_level = atoi(argv[++i]);
        else { fprintf(stderr, "unknown arg: %s\n", a.c_str()); return 2; }
    }

    if (!net_init()) {
        fprintf(stderr, "net_init failed\n");
        return 1;
    }

    int rc = 0;
    if (selftest) {
        rc = run_selftest();
    } else {
        Config cfg;
        if (!cfg.load(cfg_path)) {
            fprintf(stderr, "config '%s' not found or no anchors; "
                            "see cloud.ini.example\n", cfg_path.c_str());
            rc = 2;
        } else {
            if (log_level >= 0) cfg.log_level = log_level;
            g_log_level = (LogLevel)cfg.log_level;
            if (no_mqtt) cfg.mqtt_enable = false;
            if (!dl_mode_override.empty()) cfg.dl_mode = dl_mode_override;

            CloudServer server(cfg);
            std::unique_ptr<WebDashboard> web;
            if (!server.start()) {
                rc = 1;
            } else {
                signal(SIGINT, on_sig);
#ifndef _WIN32
                signal(SIGTERM, on_sig);
#endif
                if (cfg.web_enable && !no_web) {
                    web = std::make_unique<WebDashboard>(server, cfg.web_www_dir);
                    web->start(cfg.web_port);
                }
                fprintf(stderr, "vwise-cloud-core running (Ctrl+C to stop)\n");
                while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
                fprintf(stderr, "\nstopping...\n");
                if (web) web->stop();
                server.stop();
            }
        }
    }

    net_cleanup();
    return rc;
}
