#pragma once

#include "config.hpp"
#include "inverter_model.hpp"
#include "timer.hpp"
#include "v5_client.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Dispatcher;
class History;
class InverterState;

// Polls the inverter through the logger and runs inverter commands.
//
// One timer, every poll_period_ms: it drives the V5 client and queues the
// register reads of one cycle there. Commands requested through
// InverterState go to the head of the same queue. Results land in
// InverterState (every cycle) and History (one sample per `sample_period`);
// nothing waits for them. Dispatcher thread only.
class Poller {
public:
    Poller(Dispatcher& dispatcher, const LoggerConfig& cfg,
           const InverterModel& model, InverterState& state, History& history,
           std::chrono::milliseconds sample_period);
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

private:
    using Clock = std::chrono::steady_clock;

    // The reads of one cycle. Each tick makes a new one, owned by a
    // shared_ptr that every queued read's callback holds: it lives until
    // the last read is answered (or failed) and the cycle is finished.
    // A slow cycle may thus overlap the next one; each keeps its own bank
    // and counter, and the FIFO queue finishes them in order.
    struct Cycle {
        Clock::time_point start;
        RegisterBank      bank;
        size_t            left = 0;
        std::string       error;   // first failure, empty if none
    };

    void on_tick();
    void start_cycle();
    void on_block(Cycle& c, const RegisterBlock& b, bool identity, bool ok,
                  const std::vector<uint8_t>& modbus, const std::string& error);
    void block_done(Cycle& c, std::string error);
    void finish_cycle(Cycle& c);
    void publish_identity(const RegisterBank& bank);

    void start_command(const InverterCommand& cmd);
    void on_command_read(const InverterCommand& cmd, bool ok,
                         const std::vector<uint8_t>& modbus, const std::string& error);
    void on_command_write(const InverterCommand& cmd, CommandResult r, bool ok,
                          const std::vector<uint8_t>& modbus, const std::string& error);
    void on_command_verify(const InverterCommand& cmd, CommandResult r, bool ok,
                           const std::vector<uint8_t>& modbus, const std::string& error);
    void command_done(const InverterCommand& cmd, CommandResult r);

    Dispatcher&               dispatcher_;
    LoggerConfig              cfg_;
    const InverterModel&      model_;
    InverterState&            state_;
    History&                  history_;
    std::chrono::milliseconds sample_period_;
    V5Client                  client_;
    Timer                     timer_;

    // Cycles keep failing; only the first one is logged as a warning.
    bool failing_        = false;
    std::optional<Clock::time_point> last_stored_;
};
