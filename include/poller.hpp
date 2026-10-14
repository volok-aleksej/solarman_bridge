#pragma once

#include "config.hpp"
#include "deye_registers.hpp"
#include "timer.hpp"
#include "v5_client.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Dispatcher;
class History;
class InverterState;

// Polls the inverter through the logger every poll_period_ms and publishes
// each cycle to InverterState and History. A state machine on the
// Dispatcher thread, with no thread of its own and no blocking waits:
//
//   Idle --cycle timer--> [connect] --> read block 0..n --> Idle
//
// The identity block is read once per connection, before the first cycle.
// Any failure ends the cycle; after a lost connection the next cycle starts
// reconnect_ms after the failed one began, otherwise poll_period_ms.
class Poller {
public:
    Poller(Dispatcher& dispatcher, const LoggerConfig& cfg,
           InverterState& state, History& history);
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

private:
    using Clock = std::chrono::steady_clock;

    void start_cycle();
    void on_connected(bool ok, const std::string& error);
    void read_next();
    void on_reply(bool ok, std::vector<uint8_t> modbus, const std::string& error);
    void publish_identity();
    void publish_sample();
    void end_cycle(bool ok, const std::string& error);

    Dispatcher&    dispatcher_;
    LoggerConfig   cfg_;
    InverterState& state_;
    History&       history_;
    V5Client       client_;
    Timer          cycle_timer_;
    Timer          purge_timer_;

    // Current cycle
    Clock::time_point          cycle_start_{};
    std::vector<RegisterBlock> blocks_;
    size_t                     next_block_ = 0;
    RegisterBank               bank_;

    bool identity_known_ = false;
    // Log the first failure of a streak loudly, the repeats quietly, so an
    // inverter that is off for the night does not flood the log.
    bool failing_ = false;
};
