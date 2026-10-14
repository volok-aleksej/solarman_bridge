#pragma once

#include "config.hpp"
#include "event.hpp"
#include "thread.hpp"
#include "v5_client.hpp"

#include <chrono>
#include <optional>
#include <string>

class History;
class InverterState;

// Worker thread: polls the inverter through the logger every
// poll_period_ms, publishes each cycle to InverterState and History.
class Poller : public Thread {
public:
    Poller(const LoggerConfig& cfg, InverterState& state, History& history);

protected:
    void run() override;
    void on_stop() override { stop_event_.signal(); }

private:
    bool poll_identity(std::string& error);
    bool poll_cycle(std::string& error);
    // Sleep until `deadline` or until stop() is called.
    void sleep_until(std::chrono::steady_clock::time_point deadline);

    LoggerConfig   cfg_;
    InverterState& state_;
    History&       history_;
    Event          stop_event_;
    V5Client       client_;
    bool           identity_known_ = false;
};
