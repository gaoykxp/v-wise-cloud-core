/**********************************************************************************************************************
    > File: cloud_config.cpp
**********************************************************************************************************************/
#include "cloud_config.h"
#include "logger.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace vwise {
namespace cloud {

static std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool Config::load(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k.empty()) continue;
        kv_[section + "." + k] = v;
    }

    /* 派生字段 */
    uint16_t def_port = 39001;
    std::string anchors_s = get("server.anchors", "0.0.0.0:39001,0.0.0.0:39002");
    std::string cur;
    for (size_t i = 0; i <= anchors_s.size(); ++i) {
        if (i == anchors_s.size() || anchors_s[i] == ',') {
            Endpoint e = parse_endpoint(trim(cur), def_port);
            if (e.valid) anchors.push_back(e);
            cur.clear();
        } else {
            cur += anchors_s[i];
        }
    }
    stats_interval_sec = atof(get("server.stats_interval_sec", "2").c_str());

    mqtt_enable = atoi(get("mqtt.enable", "0").c_str()) != 0;
    mqtt_host = get("mqtt.host", "127.0.0.1");
    mqtt_port = (uint16_t)atoi(get("mqtt.port", "1883").c_str());
    mqtt_client_id = get("mqtt.client_id", "vwise_cloud_core");
    mqtt_status_filter = get("mqtt.status_filter", "vwise/+/link/status/up");
    mqtt_mode_prefix = get("mqtt.mode_prefix", "vwise/");

    dl_mode = get("downlink.mode", "dual");
    dl_rate_pps = atof(get("downlink.rate_pps", "50").c_str());
    dl_payload_bytes = atoi(get("downlink.payload_bytes", "200").c_str());
    dl_flow_id = (uint32_t)strtoul(get("downlink.flow_id", "0x5100").c_str(), nullptr, 0);

    web_enable = atoi(get("web.enable", "1").c_str()) != 0;
    web_port = (uint16_t)atoi(get("web.port", "8080").c_str());
    web_www_dir = get("web.www_dir", "www");

    log_level = atoi(get("log.level", "1").c_str());

    return !anchors.empty();
}

std::string Config::get(const std::string &key, const std::string &def) const {
    auto it = kv_.find(key);
    return it != kv_.end() ? it->second : def;
}

} // namespace cloud
} // namespace vwise
