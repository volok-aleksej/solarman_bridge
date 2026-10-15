#include "http_server.hpp"

#include "history.hpp"
#include "inverter_model.hpp"
#include "inverter_state.hpp"
#include "sample_columns.hpp"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>

#include <cjson/cJSON.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {

constexpr int kMaxHistoryPoints = 2000;
constexpr int kDefaultHistoryPoints = 600;

struct CJsonPtr {
    cJSON* p;
    explicit CJsonPtr(cJSON* p) : p(p) {}
    ~CJsonPtr() { cJSON_Delete(p); }
    CJsonPtr(const CJsonPtr&) = delete;
    CJsonPtr& operator=(const CJsonPtr&) = delete;
};

std::string cjson_print(cJSON* obj) {
    char* raw = cJSON_PrintUnformatted(obj);
    if (!raw) return {};
    std::string out(raw);
    cJSON_free(raw);
    return out;
}

int64_t epoch_ms(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               t.time_since_epoch()).count();
}

void add_columns(cJSON* o, const InverterSample& s,
                 const SampleColumn* begin, const SampleColumn* end) {
    for (const SampleColumn* c = begin; c != end; ++c)
        cJSON_AddNumberToObject(o, c->name, c->get(s));
}

cJSON* sample_to_cjson(const InverterSample& s, bool with_counters) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ts", epoch_ms(s.ts));
    add_columns(o, s, std::begin(kSampleColumns), std::end(kSampleColumns));
    if (with_counters)
        add_columns(o, s, std::begin(kCounterColumns), std::end(kCounterColumns));
    return o;
}

// Query string of the request as a parsed key/value list. Owns the list.
struct Query {
    evkeyvalq params{};
    explicit Query(evhttp_request* req) {
        const evhttp_uri* uri = evhttp_request_get_evhttp_uri(req);
        const char* q = uri ? evhttp_uri_get_query(uri) : nullptr;
        if (q) evhttp_parse_query_str(q, &params);
    }
    ~Query() { evhttp_clear_headers(&params); }
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;

    long long num(const char* key, long long def) const {
        const char* v = evhttp_find_header(&params, key);
        return v && v[0] ? std::atoll(v) : def;
    }
    const char* str(const char* key) const { return evhttp_find_header(&params, key); }
};

constexpr size_t kMaxBodySize = 1024;


const char* mime_for_ext(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")                   return "text/css";
    if (ext == ".js")                    return "application/javascript";
    if (ext == ".json")                  return "application/json";
    if (ext == ".svg")                   return "image/svg+xml";
    if (ext == ".png")                   return "image/png";
    if (ext == ".ico")                   return "image/x-icon";
    if (ext == ".txt")                   return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

}  // namespace

HttpServer::HttpServer(const History& history, std::vector<InverterView> inverters,
                       const HttpConfig& http_cfg, const ControlConfig& control_cfg)
    : history_(history), inverters_(std::move(inverters)),
      control_enabled_(control_cfg.enabled),
      port_(http_cfg.http_port), www_root_(http_cfg.www_root) {
    evthread_use_pthreads();
    base_ = event_base_new();
    if (!base_) throw std::runtime_error("http: event_base_new failed");

    http_ = evhttp_new(base_);
    if (!http_) throw std::runtime_error("http: evhttp_new failed");
    evhttp_set_allowed_methods(http_, EVHTTP_REQ_GET | EVHTTP_REQ_HEAD |
                                      EVHTTP_REQ_POST);
    evhttp_set_max_body_size(http_, kMaxBodySize);
    evhttp_set_gencb(http_, on_request, this);

    if (evhttp_bind_socket(http_, "0.0.0.0", port_) != 0)
        throw std::runtime_error("http: cannot bind port " + std::to_string(port_));
}

HttpServer::~HttpServer() {
    stop();
    if (http_) evhttp_free(http_);
    if (base_) event_base_free(base_);
}

void HttpServer::run() {
    spdlog::info("http: listening on 0.0.0.0:{}", port_);
    event_base_dispatch(base_);
    spdlog::info("http: event loop stopped");
}

void HttpServer::on_stop() {
    event_base_loopbreak(base_);
}

void HttpServer::on_request(evhttp_request* req, void* arg) {
    auto* self = static_cast<HttpServer*>(arg);
    const evhttp_uri* uri = evhttp_request_get_evhttp_uri(req);
    const char* p = uri ? evhttp_uri_get_path(uri) : nullptr;
    std::string path = p && p[0] ? p : "/";
    if (path == "/") path = "/index.html";

    const bool post = evhttp_request_get_command(req) == EVHTTP_REQ_POST;
    if (post != (path == "/api/command")) {
        send_json(req, HTTP_BADMETHOD, "{\"error\":\"method not allowed\"}");
        return;
    }

    if      (path == "/api/command")   self->handle_command(req);
    else if (path == "/api/inverters") self->handle_inverters(req);
    else if (path == "/api/live")      self->handle_live(req);
    else if (path == "/api/info")    self->handle_info(req);
    else if (path == "/api/history") self->handle_history(req);
    else if (path == "/api/daily")   self->handle_daily(req);
    else                             self->serve_file(req, path);
}

void HttpServer::send_json(evhttp_request* req, int code, std::string body) {
    evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, body.data(), body.size());
    evkeyvalq* h = evhttp_request_get_output_headers(req);
    evhttp_add_header(h, "Content-Type", "application/json");
    evhttp_add_header(h, "Cache-Control", "no-store");
    evhttp_send_reply(req, code, code == HTTP_OK ? "OK" : "Error", buf);
    evbuffer_free(buf);
}

void HttpServer::serve_file(evhttp_request* req, const std::string& path) {
    const std::string full = www_root_ + path;

    char resolved[PATH_MAX];
    char root_resolved[PATH_MAX];
    if (!realpath(full.c_str(), resolved) ||
        !realpath(www_root_.c_str(), root_resolved)) {
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
        return;
    }
    const std::string root_s = std::string(root_resolved) + '/';
    if (std::string(resolved).compare(0, root_s.size(), root_s) != 0) {
        spdlog::warn("http: path traversal attempt: {}", path);
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
        return;
    }

    std::ifstream f(resolved, std::ios::binary);
    if (!f) {
        evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
        return;
    }
    std::string body((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

    evbuffer* buf = evbuffer_new();
    evbuffer_add(buf, body.data(), body.size());
    evhttp_add_header(evhttp_request_get_output_headers(req),
                      "Content-Type", mime_for_ext(resolved));
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
    evbuffer_free(buf);
}

const InverterView* HttpServer::pick(evhttp_request* req, const char* name) {
    if (!name || !name[0]) return &inverters_.front();
    for (const InverterView& v : inverters_)
        if (v.cfg.name == name) return &v;
    send_json(req, HTTP_NOTFOUND, "{\"error\":\"unknown inverter\"}");
    return nullptr;
}

void HttpServer::handle_inverters(evhttp_request* req) {
    CJsonPtr root(cJSON_CreateObject());
    cJSON* arr = cJSON_AddArrayToObject(root.p, "inverters");
    for (const InverterView& v : inverters_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name",        v.cfg.name.c_str());
        cJSON_AddStringToObject(o, "model",       v.model.name);
        cJSON_AddStringToObject(o, "description", v.model.description);
        cJSON_AddItemToArray(arr, o);
    }
    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_live(evhttp_request* req) {
    const Query q(req);
    const InverterView* v = pick(req, q.str("inverter"));
    if (!v) return;
    const InverterSnapshot snap = v->state.snapshot();
    CJsonPtr root(cJSON_CreateObject());

    // "online": the link is up and the last sample is recent enough
    // (three missed cycles and the data is stale).
    int64_t age_ms = -1;
    if (snap.sample)
        age_ms = epoch_ms(std::chrono::system_clock::now()) - epoch_ms(snap.sample->ts);
    const bool online = snap.connected && age_ms >= 0 &&
                        age_ms <= 3LL * v->cfg.poll_period_ms;

    cJSON_AddBoolToObject  (root.p, "online",    online);
    cJSON_AddBoolToObject  (root.p, "connected", snap.connected);
    cJSON_AddNumberToObject(root.p, "age_ms",    age_ms);
    cJSON_AddStringToObject(root.p, "inverter",  v->cfg.name.c_str());
    cJSON_AddNumberToObject(root.p, "poll_period_ms", v->cfg.poll_period_ms);
    if (snap.sample) {
        cJSON_AddStringToObject(root.p, "run_state_name",
                                v->model.state_name(snap.sample->run_state));
        cJSON_AddBoolToObject  (root.p, "running", v->model.running(snap.sample->run_state));
        cJSON_AddItemToObject(root.p, "sample", sample_to_cjson(*snap.sample, true));
    }
    if (snap.last_command) {
        const CommandStatus& c = *snap.last_command;
        cJSON* lc = cJSON_AddObjectToObject(root.p, "last_command");
        cJSON_AddStringToObject(lc, "name", c.name.c_str());
        cJSON_AddNumberToObject(lc, "at",   epoch_ms(c.at));
        cJSON_AddBoolToObject  (lc, "ok",   c.result.ok);
        if (c.result.ok) {
            cJSON_AddNumberToObject(lc, "before", c.result.before);
            cJSON_AddNumberToObject(lc, "after",  c.result.after);
        } else {
            cJSON_AddStringToObject(lc, "error", c.result.error.c_str());
        }
    }
    if (snap.clock) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d", snap.clock->date().c_str(),
                 snap.clock->hour, snap.clock->minute, snap.clock->second);
        cJSON_AddStringToObject(root.p, "inverter_time", buf);
    }
    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_info(evhttp_request* req) {
    const Query q(req);
    const InverterView* v = pick(req, q.str("inverter"));
    if (!v) return;
    const InverterSnapshot snap = v->state.snapshot();
    CJsonPtr root(cJSON_CreateObject());

    cJSON* lg = cJSON_AddObjectToObject(root.p, "logger");
    cJSON_AddStringToObject(lg, "host",      v->cfg.host.c_str());
    cJSON_AddNumberToObject(lg, "port",      v->cfg.port);
    cJSON_AddNumberToObject(lg, "serial",    v->cfg.serial);
    cJSON_AddBoolToObject  (lg, "connected", snap.connected);

    cJSON* inv = cJSON_AddObjectToObject(root.p, "inverter");
    cJSON_AddStringToObject(inv, "name",           v->cfg.name.c_str());
    cJSON_AddStringToObject(inv, "model",          v->model.name);
    cJSON_AddStringToObject(inv, "description",    v->model.description);
    cJSON_AddNumberToObject(inv, "slave_id",       v->cfg.slave_id);
    cJSON_AddNumberToObject(inv, "poll_period_ms", v->cfg.poll_period_ms);
    if (snap.identity) {
        cJSON_AddStringToObject(inv, "serial",      snap.identity->serial.c_str());
        cJSON_AddNumberToObject(inv, "device_type", snap.identity->device_type);
    }

    cJSON* ctl = cJSON_AddObjectToObject(root.p, "control");
    cJSON_AddBoolToObject(ctl, "enabled", control_enabled_);
    cJSON* cmds = cJSON_AddArrayToObject(ctl, "commands");
    for (const auto& c : v->model.commands) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name",        c.name);
        cJSON_AddStringToObject(o, "description", c.description);
        cJSON_AddItemToArray(cmds, o);
    }

    cJSON* st = cJSON_AddObjectToObject(root.p, "stats");
    cJSON_AddNumberToObject(st, "cycles_ok",     snap.stats.cycles_ok);
    cJSON_AddNumberToObject(st, "cycles_failed", snap.stats.cycles_failed);
    cJSON_AddNumberToObject(st, "reconnects",    snap.stats.reconnects);
    cJSON_AddNumberToObject(st, "last_cycle_ms", snap.stats.last_cycle_ms);
    cJSON_AddStringToObject(st, "last_error",    snap.stats.last_error.c_str());
    if (!snap.stats.last_error.empty())
        cJSON_AddNumberToObject(st, "last_error_at",
                                epoch_ms(snap.stats.last_error_at));
    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_history(evhttp_request* req) {
    const Query q(req);
    const InverterView* v = pick(req, q.str("inverter"));
    if (!v) return;
    const long long now_s = time(nullptr);
    const long long t_end   = q.num("time_end",   now_s);
    const long long t_start = q.num("time_start", t_end - 86400);
    long long count = q.num("count", kDefaultHistoryPoints);
    if (count <= 0 || count > kMaxHistoryPoints) count = kMaxHistoryPoints;
    if (t_start >= t_end) {
        send_json(req, HTTP_BADREQUEST,
                  "{\"error\":\"time_start must be before time_end\"}");
        return;
    }

    const auto data = history_.range(
        v->history_id, std::chrono::system_clock::from_time_t(t_start),
        std::chrono::system_clock::from_time_t(t_end), count);

    CJsonPtr root(cJSON_CreateObject());
    cJSON* arr = cJSON_AddArrayToObject(root.p, "data");
    for (const auto& s : data)
        cJSON_AddItemToArray(arr, sample_to_cjson(s, false));
    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_daily(evhttp_request* req) {
    const Query q(req);
    // inverter=all: every inverter, summed per day (history id 0).
    const char* name = q.str("inverter");
    int history_id = 0;
    if (!name || std::string(name) != "all") {
        const InverterView* v = pick(req, name);
        if (!v) return;
        history_id = v->history_id;
    }
    const time_t now_t = time(nullptr);
    tm lt{};
    localtime_r(&now_t, &lt);
    const int year  = q.num("year",  lt.tm_year + 1900);
    const int month = q.num("month", lt.tm_mon + 1);
    if (year < 2000 || year > 2100 || month < 1 || month > 12) {
        send_json(req, HTTP_BADREQUEST, "{\"error\":\"bad year/month\"}");
        return;
    }

    CJsonPtr root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.p, "year",  year);
    cJSON_AddNumberToObject(root.p, "month", month);
    cJSON* arr = cJSON_AddArrayToObject(root.p, "data");
    for (const auto& e : history_.daily(history_id, year, month)) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "date",              e.date.c_str());
        cJSON_AddNumberToObject(o, "pv_wh",             e.pv_wh);
        cJSON_AddNumberToObject(o, "load_wh",           e.load_wh);
        cJSON_AddNumberToObject(o, "grid_buy_wh",       e.grid_buy_wh);
        cJSON_AddNumberToObject(o, "grid_sell_wh",      e.grid_sell_wh);
        cJSON_AddNumberToObject(o, "batt_charge_wh",    e.batt_charge_wh);
        cJSON_AddNumberToObject(o, "batt_discharge_wh", e.batt_discharge_wh);
        cJSON_AddItemToArray(arr, o);
    }
    send_json(req, HTTP_OK, cjson_print(root.p));
}

void HttpServer::handle_command(evhttp_request* req) {
    if (!control_enabled_) {
        send_json(req, 403,
                  "{\"error\":\"control is disabled in the config\"}");
        return;
    }
    // A JSON body is not a "simple" cross-origin request, so a foreign page
    // cannot fire a command without a CORS preflight, which is not served.
    const char* ctype = evhttp_find_header(evhttp_request_get_input_headers(req),
                                           "Content-Type");
    if (!ctype || std::string(ctype).compare(0, 16, "application/json") != 0) {
        send_json(req, 415, "{\"error\":\"expected application/json\"}");
        return;
    }

    evbuffer* in = evhttp_request_get_input_buffer(req);
    std::string body(evbuffer_get_length(in), '\0');
    evbuffer_copyout(in, body.data(), body.size());
    CJsonPtr json(cJSON_Parse(body.c_str()));
    const cJSON* inv  = cJSON_GetObjectItemCaseSensitive(json.p, "inverter");
    const InverterView* v = pick(req, cJSON_IsString(inv) ? inv->valuestring : nullptr);
    if (!v) return;
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(json.p, "name");
    const InverterCommand* cmd =
        cJSON_IsString(name) ? v->model.find_command(name->valuestring) : nullptr;
    if (!cmd) {
        send_json(req, HTTP_BADREQUEST, "{\"error\":\"unknown command\"}");
        return;
    }

    char* peer = nullptr;
    uint16_t peer_port = 0;
    evhttp_connection_get_peer(evhttp_request_get_connection(req), &peer, &peer_port);
    spdlog::info("http: command {} for {} from {}", cmd->name, v->cfg.name,
                 peer ? peer : "?");

    if (!v->state.request_command(*cmd)) {
        send_json(req, HTTP_SERVUNAVAIL, "{\"error\":\"too many pending commands\"}");
        return;
    }
    // The poller picks it up on its next tick; the outcome shows up in
    // api/live as "last_command".
    send_json(req, 202, "{\"queued\":true}");
}
