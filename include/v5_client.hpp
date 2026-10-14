#pragma once

#include "timer.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Dispatcher;

// Asynchronous Solarman V5 transport on the Dispatcher: one persistent TCP
// connection to the logger, one request in flight. It carries opaque Modbus
// RTU frames; what to ask and how to read the answer is up to the caller.
//
// Unsolicited counter frames and stale replies are skipped. A timeout, a
// malformed frame or a socket error closes the connection and fails the
// pending operation; the caller reconnects when it wants to.
//
// Completion callbacks run on the dispatcher thread after the client is
// back to its resting state, so they may start the next request or close().
class V5Client {
public:
    using Done  = std::function<void(bool ok, const std::string& error)>;
    using Reply = std::function<void(bool ok, std::vector<uint8_t> modbus,
                                     const std::string& error)>;

    V5Client(Dispatcher& dispatcher, std::string host, uint16_t port,
             uint32_t logger_serial, std::chrono::milliseconds timeout);
    ~V5Client();

    V5Client(const V5Client&) = delete;
    V5Client& operator=(const V5Client&) = delete;

    bool connected() const noexcept {
        return state_ == State::Idle || state_ == State::Awaiting;
    }
    bool idle() const noexcept { return state_ == State::Idle; }

    // Requires the client to be closed. The host is resolved with
    // getaddrinfo, which blocks only when it is a name, not an address.
    void connect(Done done);
    // Requires idle().
    void request(const std::vector<uint8_t>& modbus_frame, Reply done);
    // Drops the connection; a pending callback is not called.
    void close();

private:
    enum class State { Closed, Connecting, Idle, Awaiting };

    void on_socket(uint32_t events);
    void on_timer();
    void finish_connect();
    bool flush_tx();
    void read_socket();
    void process_rx();
    // Report `error` asynchronously, from the timer, so that connect() and
    // request() never call back before they return.
    void defer_error(std::string error);
    void fail(std::string error);

    Dispatcher&               dispatcher_;
    std::string               host_;
    uint16_t                  port_;
    uint32_t                  logger_serial_;
    std::chrono::milliseconds timeout_;

    State                state_ = State::Closed;
    int                  fd_    = -1;
    uint32_t             watched_events_ = 0;
    uint8_t              seq_   = 0;
    Timer                timer_;
    std::string          deferred_error_;
    std::vector<uint8_t> tx_;
    std::vector<uint8_t> rx_;
    Done                 connect_done_;
    Reply                reply_;
};
