#pragma once

#include "config.hpp"
#include "thread.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct event_base;
struct evhttp;
struct evhttp_request;

class History;
class InverterState;
struct InverterModel;

// What the HTTP thread sees of one inverter.
struct InverterView {
    const InverterConfig& cfg;
    const InverterModel&  model;
    InverterState&        state;
    int                   history_id;
};

// Dashboard + JSON API on its own libevent thread.
//
//   GET api/inverters                     the configured inverters
//   GET api/live?inverter                 latest sample + link status
//   GET api/info?inverter                 logger / inverter identity, poll stats
//   GET api/history?inverter&time_start&time_end&count   decimated samples
//   GET api/daily?inverter&year&month     daily energy of one month; inverter=all sums
//   POST api/command {"inverter", "name"} queue an inverter command (if enabled)
//
// `inverter` is a section title from the config; without it, the first one.
//   anything else                         static file under www_root
//
// Requests only read what the poller has stored; a command is queued and
// its outcome later shows up in api/live.
//
// The pages use relative URLs, so the service also works behind a reverse
// proxy that mounts it under a path prefix.
class HttpServer : public Thread {
public:
    HttpServer(const History& history, std::vector<InverterView> inverters,
               const HttpConfig& http_cfg, const ControlConfig& control_cfg);
    ~HttpServer() override;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

protected:
    void run() override;
    void on_stop() override;

private:
    static void on_request(struct evhttp_request* req, void* self);
    // The inverter a request names, or nullptr after answering 404.
    const InverterView* pick(struct evhttp_request* req, const char* name);
    void handle_inverters(struct evhttp_request* req);
    void handle_live(struct evhttp_request* req);
    void handle_info(struct evhttp_request* req);
    void handle_history(struct evhttp_request* req);
    void handle_daily(struct evhttp_request* req);
    void handle_command(struct evhttp_request* req);
    void serve_file(struct evhttp_request* req, const std::string& path);
    static void send_json(struct evhttp_request* req, int code, std::string body);

    const History&       history_;
    std::vector<InverterView> inverters_;
    bool                 control_enabled_;
    uint16_t             port_;
    std::string          www_root_;

    event_base* base_ = nullptr;
    evhttp*     http_ = nullptr;
};
