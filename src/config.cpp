#include "config.hpp"

#include <confuse.h>

#include <cerrno>
#include <cstring>

namespace {

// Range check for integer options; registered per option below.
template <long Min, long Max>
int vf_range(cfg_t* cfg, cfg_opt_t* opt) {
    const long v = cfg_opt_getnint(opt, 0);
    if (v < Min || v > Max) {
        cfg_error(cfg, "'%s' must be in [%ld..%ld], got %ld",
                  cfg_opt_name(opt), Min, Max, v);
        return -1;
    }
    return 0;
}

cfg_opt_t logger_opts[] = {
    CFG_STR("host",           nullptr, CFGF_NODEFAULT),
    CFG_INT("serial",         0,       CFGF_NODEFAULT),
    CFG_INT("port",           8899,    CFGF_NONE),
    CFG_INT("slave_id",       1,       CFGF_NONE),
    CFG_INT("timeout_ms",     5000,    CFGF_NONE),
    CFG_INT("poll_period_ms", 5000,    CFGF_NONE),
    CFG_INT("reconnect_ms",   10000,   CFGF_NONE),
    CFG_END()
};

cfg_opt_t http_opts[] = {
    CFG_INT("port", 8081, CFGF_NONE),
    CFG_STR("www_root", "/usr/share/solarman_bridge/www", CFGF_NONE),
    CFG_END()
};

cfg_opt_t history_opts[] = {
    CFG_STR("db_path", "/var/lib/solarman_bridge/history.sqlite3", CFGF_NONE),
    CFG_INT("retention_days", 30, CFGF_NONE),
    CFG_END()
};

std::string sstr(cfg_t* s, const char* name) {
    const char* v = cfg_getstr(s, name);
    return v ? std::string(v) : std::string();
}

}  // namespace

bool load_config(const std::string& path, AppConfig& out, std::string& error) {
    cfg_opt_t opts[] = {
        CFG_INT("log_level", 4,  CFGF_NONE),
        CFG_STR("log_file",  "", CFGF_NONE),
        CFG_SEC("logger",  logger_opts,  CFGF_NONE),
        CFG_SEC("http",    http_opts,    CFGF_NONE),
        CFG_SEC("history", history_opts, CFGF_NONE),
        CFG_END()
    };

    cfg_t* cfg = cfg_init(opts, CFGF_NONE);
    cfg_set_validate_func(cfg, "log_level",              vf_range<1, 5>);
    cfg_set_validate_func(cfg, "logger|serial",          vf_range<1, 0xFFFFFFFFL>);
    cfg_set_validate_func(cfg, "logger|port",            vf_range<1, 65535>);
    cfg_set_validate_func(cfg, "logger|slave_id",        vf_range<1, 247>);
    cfg_set_validate_func(cfg, "logger|timeout_ms",      vf_range<500, 60000>);
    cfg_set_validate_func(cfg, "logger|poll_period_ms",  vf_range<1000, 3600000>);
    cfg_set_validate_func(cfg, "logger|reconnect_ms",    vf_range<1000, 3600000>);
    cfg_set_validate_func(cfg, "http|port",              vf_range<1, 65535>);
    cfg_set_validate_func(cfg, "history|retention_days", vf_range<1, 3650>);

    int rc = cfg_parse(cfg, path.c_str());
    if (rc == CFG_FILE_ERROR) {
        error = "cannot open config file: " + path + " (" + std::strerror(errno) + ")";
        cfg_free(cfg);
        return false;
    }
    if (rc == CFG_PARSE_ERROR) {
        error = "parse/validation error in config file: " + path;
        cfg_free(cfg);
        return false;
    }

    auto fail = [&](std::string msg) {
        error = "config: " + std::move(msg);
        cfg_free(cfg);
        return false;
    };

    out.log_level = cfg_getint(cfg, "log_level");
    out.log_file  = sstr(cfg, "log_file");

    cfg_t* lg = cfg_getsec(cfg, "logger");
    out.logger.host = sstr(lg, "host");
    if (out.logger.host.empty()) return fail("logger { host = ... } is required");
    if (cfg_size(lg, "serial") == 0)
        return fail("logger { serial = ... } is required");
    out.logger.serial         = cfg_getint(lg, "serial");
    out.logger.port           = cfg_getint(lg, "port");
    out.logger.slave_id       = cfg_getint(lg, "slave_id");
    out.logger.timeout_ms     = cfg_getint(lg, "timeout_ms");
    out.logger.poll_period_ms = cfg_getint(lg, "poll_period_ms");
    out.logger.reconnect_ms   = cfg_getint(lg, "reconnect_ms");

    if (cfg_t* s = cfg_getsec(cfg, "http")) {
        out.http.http_port = cfg_getint(s, "port");
        out.http.www_root  = sstr(s, "www_root");
    }
    if (cfg_t* s = cfg_getsec(cfg, "history")) {
        out.history.db_path        = sstr(s, "db_path");
        out.history.retention_days = cfg_getint(s, "retention_days");
    }

    cfg_free(cfg);
    return true;
}
