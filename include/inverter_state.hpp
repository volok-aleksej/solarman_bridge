#pragma once

#include "inverter_data.hpp"
#include "inverter_commands.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

struct PollStats {
    uint64_t cycles_ok     = 0;
    uint64_t cycles_failed = 0;
    uint64_t reconnects    = 0;
    int64_t  last_cycle_ms = 0;   // duration of the last successful cycle
    std::string last_error;
    std::chrono::system_clock::time_point last_error_at{};
};

// Outcome of the last inverter command.
struct CommandStatus {
    std::string   name;
    CommandResult result;
    std::chrono::system_clock::time_point at;
};

// Snapshot of everything the poller has learned about the inverter.
struct InverterSnapshot {
    bool connected = false;
    std::optional<InverterIdentity> identity;
    std::optional<InverterSample>   sample;
    std::optional<InverterClock>    clock;
    std::optional<CommandStatus>    last_command;
    PollStats stats;
};

// Written by the poller on the dispatcher thread, read by the HTTP thread.
// The HTTP thread also leaves commands here for the poller to pick up.
class InverterState {
public:
    // Called on every tick; acts on changes only. `error` is why the link
    // went down.
    void set_connected(bool v, const std::string& error) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (v == snap_.connected) return;
        snap_.connected = v;
        if (v) {
            ++snap_.stats.reconnects;
        } else {
            snap_.stats.last_error    = error;
            snap_.stats.last_error_at = std::chrono::system_clock::now();
        }
    }

    bool has_identity() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return snap_.identity.has_value();
    }

    void set_identity(InverterIdentity id) {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.identity = std::move(id);
    }

    void apply_cycle(InverterSample s, std::optional<InverterClock> c,
                     int64_t cycle_ms) {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.sample = std::move(s);
        snap_.clock  = std::move(c);
        snap_.stats.last_cycle_ms = cycle_ms;
        ++snap_.stats.cycles_ok;
    }

    void record_failure(std::string error) {
        std::lock_guard<std::mutex> lk(mtx_);
        ++snap_.stats.cycles_failed;
        snap_.stats.last_error    = std::move(error);
        snap_.stats.last_error_at = std::chrono::system_clock::now();
    }

    // HTTP thread. False when kMaxPendingCommands are already waiting.
    bool request_command(const InverterCommand& cmd) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pending_.size() >= kMaxPendingCommands) return false;
        pending_.push_back(&cmd);
        return true;
    }

    // Poller: the commands requested since the last call.
    std::deque<const InverterCommand*> take_commands() {
        std::lock_guard<std::mutex> lk(mtx_);
        return std::exchange(pending_, {});
    }

    void set_command(std::string name, CommandResult r) {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.last_command = CommandStatus{std::move(name), std::move(r),
                                           std::chrono::system_clock::now()};
    }

    InverterSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return snap_;
    }

private:
    static constexpr size_t kMaxPendingCommands = 4;

    mutable std::mutex mtx_;
    InverterSnapshot snap_;
    std::deque<const InverterCommand*> pending_;
};
