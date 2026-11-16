/**********************************************************************************************************************
    > File: cloud_config.h / cloud_config.cpp 合并说明
    > ini 风格配置读取（零依赖）：[section] key = value
**********************************************************************************************************************/
#ifndef VWISE_CLOUD_CONFIG_H
#define VWISE_CLOUD_CONFIG_H

#include "net.h"
#include <map>
#include <string>
#include <vector>

namespace vwise {
namespace cloud {

class Config {
public:
    bool load(const std::string &path);
    std::string get(const std::string &key, const std::string &def) const;

    /* 载入后的派生字段 */
    std::vector<Endpoint> anchors;       /* [server] anchors（逗号分隔 host[:port]） */
    double stats_interval_sec = 2.0;

    /* [mqtt] */
    bool mqtt_enable = false;
    std::string mqtt_host = "127.0.0.1";
    uint16_t mqtt_port = 1883;
    std::string mqtt_client_id = "vwise_cloud_core";
    std::string mqtt_status_filter = "vwise/+/link/status/up";
    std::string mqtt_mode_prefix = "vwise/";

    /* [downlink] */
    std::string dl_mode = "dual";        /* off | best | dual */
    double dl_rate_pps = 50.0;
    int dl_payload_bytes = 200;
    uint32_t dl_flow_id = 0x5100u;

    /* [web] 内嵌看板 */
    bool web_enable = true;
    uint16_t web_port = 8080;
    std::string web_www_dir = "www";

    int log_level = 1; /* 0=dbg 1=info 2=warn 3=err */

private:
    std::map<std::string, std::string> kv_; /* "section.key" → value */
};

} // namespace cloud
} // namespace vwise

#endif // VWISE_CLOUD_CONFIG_H
