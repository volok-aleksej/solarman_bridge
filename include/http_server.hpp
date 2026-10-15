#pragma once

#include "config.hpp"
#include "thread.hpp"

#include <cstdint>
#include <string>

struct event_base;
struct evhttp;
struct evhttp_request;

class History;
class InverterState;

// Dashboard + JSON API on its own libevent thread.
//
//   GET api/live                          latest sample + link status
//   GET api/info                          logger / inverter identity, poll stats
//   GET api/history?time_start&time_end&count   decimated samples, oldest first
//   GET api/daily?year&month              daily energy rows of one month
//   POST api/command {"name": "..."}      queue an inverter command (if enabled)
//   anything else                         static file under www_root
//
// Requests only read what the poller has stored; a command is queued and
// its outcome later shows up in api/live.
//
// The pages use relative URLs, so the service also works behind a reverse
// proxy that mounts it under a path prefix.
class HttpServer : public Thread {
public:
    HttpServer(const History& history, InverterState& state,
               const LoggerConfig& logger_cfg,
               const HttpConfig& http_cfg, const ControlConfig& control_cfg);
    ~HttpServer() override;

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

protected:
    void run() override;
    void on_stop() override;

private:
    static void on_request(struct evhttp_request* req, void* self);
    void handle_live(struct evhttp_request* req);
    void handle_info(struct evhttp_request* req);
    void handle_history(struct evhttp_request* req);
    void handle_daily(struct evhttp_request* req);
    void handle_command(struct evhttp_request* req);
    void serve_file(struct evhttp_request* req, const std::string& path);
    static void send_json(struct evhttp_request* req, int code, std::string body);

    const History&       history_;
    InverterState&       state_;
    bool                 control_enabled_;
    LoggerConfig         logger_cfg_;
    uint16_t             port_;
    std::string          www_root_;

    event_base* base_ = nullptr;
    evhttp*     http_ = nullptr;
};
