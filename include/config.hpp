#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One inverter and the Solarman logger in front of it.
struct InverterConfig {
    std::string name;                     // section title; the API's ?inverter=
    std::string model;                    // InverterModel::name
    std::string host;                     // logger address
    uint16_t    port           = 8899;
    uint32_t    serial         = 0;       // logger (not inverter) serial number
    uint8_t     slave_id       = 1;
    int         timeout_ms     = 5000;
    int         poll_period_ms = 1000;
    int         reconnect_ms   = 10000;
};

struct HttpConfig {
    std::string bind = "0.0.0.0";   // 127.0.0.1 behind a reverse proxy
    int http_port = 8081;
    std::string www_root = "/usr/share/solarman_bridge/www";
};

struct HistoryConfig {
    std::string db_path = "/var/lib/solarman_bridge/history.sqlite3";
    int retention_days = 30;
    int sample_period_ms = 5000;   // one stored sample per this many ms
};

struct ControlConfig {
    // Lets POST api/command write to the inverter. Off unless set.
    bool enabled = false;
};

struct AppConfig {
    int log_level = 4;
    std::string log_file;

    std::vector<InverterConfig> inverters;   // at least one
    HttpConfig    http;
    HistoryConfig history;
    ControlConfig control;
};

bool load_config(const std::string& path, AppConfig& out, std::string& error);
