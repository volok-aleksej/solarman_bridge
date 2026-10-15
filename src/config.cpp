#include "config.hpp"

#include "inverter_model.hpp"

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

cfg_opt_t inverter_opts[] = {
    CFG_STR("model",          nullptr, CFGF_NODEFAULT),
    CFG_STR("host",           nullptr, CFGF_NODEFAULT),
    CFG_INT("serial",         0,       CFGF_NODEFAULT),
    CFG_INT("port",           8899,    CFGF_NONE),
    CFG_INT("slave_id",       1,       CFGF_NONE),
    CFG_INT("timeout_ms",     5000,    CFGF_NONE),
    CFG_INT("poll_period_ms", 1000,    CFGF_NONE),
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
    CFG_INT("sample_period_ms", 5000, CFGF_NONE),
    CFG_END()
};

cfg_opt_t control_opts[] = {
    CFG_BOOL("enabled", cfg_false, CFGF_NONE),
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
        CFG_SEC("inverter", inverter_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
        CFG_SEC("http",    http_opts,    CFGF_NONE),
        CFG_SEC("history", history_opts, CFGF_NONE),
        CFG_SEC("control", control_opts, CFGF_NONE),
        CFG_END()
    };

    cfg_t* cfg = cfg_init(opts, CFGF_NONE);
    cfg_set_validate_func(cfg, "log_level",              vf_range<1, 5>);
    cfg_set_validate_func(cfg, "inverter|serial",         vf_range<1, 0xFFFFFFFFL>);
    cfg_set_validate_func(cfg, "inverter|port",           vf_range<1, 65535>);
    cfg_set_validate_func(cfg, "inverter|slave_id",       vf_range<1, 247>);
    cfg_set_validate_func(cfg, "inverter|timeout_ms",     vf_range<500, 60000>);
    cfg_set_validate_func(cfg, "inverter|poll_period_ms", vf_range<1000, 3600000>);
    cfg_set_validate_func(cfg, "inverter|reconnect_ms",   vf_range<1000, 3600000>);
    cfg_set_validate_func(cfg, "http|port",              vf_range<1, 65535>);
    cfg_set_validate_func(cfg, "history|retention_days", vf_range<1, 3650>);
    cfg_set_validate_func(cfg, "history|sample_period_ms", vf_range<1000, 3600000>);

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

    for (unsigned i = 0; i < cfg_size(cfg, "inverter"); ++i) {
        cfg_t* sec = cfg_getnsec(cfg, "inverter", i);
        InverterConfig inv;
        inv.name = cfg_title(sec);
        const std::string where = "inverter \"" + inv.name + "\": ";
        inv.model = sstr(sec, "model");
        if (!find_model(inv.model)) {
            std::string names;
            for (const InverterModel* m : all_models())
                names += (names.empty() ? "" : ", ") + std::string(m->name);
            return fail(where + "model must be one of: " + names);
        }
        inv.host = sstr(sec, "host");
        if (inv.host.empty()) return fail(where + "host is required");
        if (cfg_size(sec, "serial") == 0) return fail(where + "serial is required");
        inv.serial         = cfg_getint(sec, "serial");
        inv.port           = cfg_getint(sec, "port");
        inv.slave_id       = cfg_getint(sec, "slave_id");
        inv.timeout_ms     = cfg_getint(sec, "timeout_ms");
        inv.poll_period_ms = cfg_getint(sec, "poll_period_ms");
        inv.reconnect_ms   = cfg_getint(sec, "reconnect_ms");
        out.inverters.push_back(std::move(inv));
    }
    if (out.inverters.empty()) return fail("at least one inverter section is required");

    if (cfg_t* s = cfg_getsec(cfg, "http")) {
        out.http.http_port = cfg_getint(s, "port");
        out.http.www_root  = sstr(s, "www_root");
    }
    if (cfg_t* s = cfg_getsec(cfg, "history")) {
        out.history.db_path        = sstr(s, "db_path");
        out.history.retention_days   = cfg_getint(s, "retention_days");
        out.history.sample_period_ms = cfg_getint(s, "sample_period_ms");
    }

    if (cfg_t* s = cfg_getsec(cfg, "control"))
        out.control.enabled = cfg_getbool(s, "enabled");

    cfg_free(cfg);
    return true;
}
