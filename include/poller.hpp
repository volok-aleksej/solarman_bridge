#pragma once

#include "config.hpp"
#include "deye_registers.hpp"
#include "event.hpp"
#include "inverter_commands.hpp"
#include "timer.hpp"
#include "v5_client.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class Dispatcher;
class History;
class InverterState;

// Polls the inverter through the logger every poll_period_ms, publishes
// each cycle to InverterState and History, and runs inverter commands.
// A state machine on the Dispatcher thread, with no thread of its own and
// no blocking waits:
//
//   Idle --cycle timer--> [connect] --> read block 0..n --> Idle
//
// The identity block is read once per connection. Any failure ends the
// cycle; after a lost connection the next cycle starts reconnect_ms after
// the failed one began, otherwise poll_period_ms.
//
// One request is in flight at a time. A queued command goes first, between
// two block reads of a cycle.
class Poller {
public:
    using CommandDone = std::function<void(const CommandResult&)>;

    // Every cycle updates InverterState; History gets one sample per
    // `sample_period`.
    Poller(Dispatcher& dispatcher, const LoggerConfig& cfg,
           InverterState& state, History& history,
           std::chrono::milliseconds sample_period);
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // Thread-safe. `done` runs later on the dispatcher thread; it is not
    // called if the poller is destroyed first. False if the queue is full.
    bool submit(const InverterCommand& cmd, CommandDone done);

private:
    using Clock = std::chrono::steady_clock;

    struct Command {
        const InverterCommand* cmd = nullptr;
        CommandDone            done;
        enum class Stage { Read, Write, Verify } stage = Stage::Read;
        CommandResult          result;
        uint16_t               target = 0;
    };

    static constexpr size_t kMaxQueuedCommands = 4;

    // Starts the next request if nothing is in flight.
    void pump();
    void on_connected(bool ok, const std::string& error);

    void start_cycle();
    void read_block();
    void on_block(bool ok, std::vector<uint8_t> modbus, const std::string& error);
    void publish_identity();
    void publish_sample();
    void end_cycle(bool ok, const std::string& error);

    void command_step();
    void on_command_reply(bool ok, std::vector<uint8_t> modbus,
                          const std::string& error);
    void finish_command(bool ok, std::string error);

    Dispatcher&    dispatcher_;
    LoggerConfig   cfg_;
    InverterState& state_;
    History&       history_;
    std::chrono::milliseconds sample_period_;
    V5Client       client_;
    Timer          cycle_timer_;
    Timer          purge_timer_;
    Event          command_event_;

    // Current cycle
    bool                       cycle_active_ = false;
    Clock::time_point          cycle_start_{};
    std::vector<RegisterBlock> blocks_;
    size_t                     next_block_ = 0;
    RegisterBank               bank_;

    // Current command, and those submitted from other threads
    std::optional<Command> command_;
    std::mutex             queue_mtx_;
    std::deque<Command>    queue_;

    std::optional<Clock::time_point> last_stored_;

    bool identity_known_ = false;
    // Log the first failure of a streak loudly, the repeats quietly, so an
    // inverter that is off for the night does not flood the log.
    bool failing_ = false;
};
