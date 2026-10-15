#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class Dispatcher;

// Solarman V5 transport on the Dispatcher: one TCP connection to the
// logger, a bounded queue of Modbus requests, one of them in flight.
// Dispatcher thread only.
//
// The owner drives it with tick() from its timer; there is no timer here.
// tick() connects when the link is down (again `reconnect` after a failed
// attempt or a lost link), and fails a connect or a request that has
// outlived `timeout`.
//
// Requests are taken only while connected. When the link goes down the
// queue is cleared and every queued request is answered with the error.
class V5Client {
public:
    using Reply = std::function<void(bool ok, std::vector<uint8_t> modbus,
                                     const std::string& error)>;

    V5Client(Dispatcher& dispatcher, std::string host, uint16_t port,
             uint32_t logger_serial, std::chrono::milliseconds timeout,
             std::chrono::milliseconds reconnect, size_t max_queue);
    ~V5Client();

    V5Client(const V5Client&) = delete;
    V5Client& operator=(const V5Client&) = delete;

    // Queues a request; an urgent one goes before all others. False, and
    // `done` is never called, when the link is down or the queue is full.
    // Otherwise `done` runs later, from the event loop.
    bool submit(std::vector<uint8_t> modbus_frame, Reply done, bool urgent);

    void tick();
    bool connected() const noexcept {
        return state_ == State::Idle || state_ == State::Awaiting;
    }
    // Why the link went down last time.
    const std::string& last_error() const noexcept { return last_error_; }

private:
    enum class State { Closed, Connecting, Idle, Awaiting };
    using Clock = std::chrono::steady_clock;

    struct Request {
        std::vector<uint8_t> frame;
        Reply                done;
    };

    void connect();
    void on_socket(uint32_t events);
    void finish_connect();
    void send_next();
    bool flush_tx();
    void read_socket();
    void process_rx();
    // Drop the link and answer the request in flight and every queued one
    // with `error`.
    void fail(const std::string& error);
    void close_socket();

    Dispatcher&               dispatcher_;
    std::string               host_;
    uint16_t                  port_;
    uint32_t                  logger_serial_;
    std::chrono::milliseconds timeout_;
    std::chrono::milliseconds reconnect_;
    size_t                    max_queue_;

    std::deque<Request>  queue_;
    State                state_ = State::Closed;
    int                  fd_    = -1;
    uint32_t             watched_events_ = 0;
    uint8_t              seq_   = 0;
    Clock::time_point    deadline_{};  // of the connect or the request in flight
    Clock::time_point    retry_at_{};  // next connect attempt
    Request              current_;
    std::vector<uint8_t> tx_;
    std::vector<uint8_t> rx_;
    std::string          last_error_;
    // The link keeps failing; only the first failure is logged as a warning.
    bool                 failing_ = false;
    // The queue keeps overflowing; only the first drop is logged.
    bool                 queue_full_logged_ = false;
};
