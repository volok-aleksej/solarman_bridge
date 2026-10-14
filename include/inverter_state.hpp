#pragma once

#include "deye_registers.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

struct PollStats {
    uint64_t cycles_ok     = 0;
    uint64_t cycles_failed = 0;
    uint64_t reconnects    = 0;
    int64_t  last_cycle_ms = 0;   // duration of the last successful cycle
    std::string last_error;
    std::chrono::system_clock::time_point last_error_at{};
};

// Snapshot of everything the poller has learned about the inverter.
struct InverterSnapshot {
    bool connected = false;
    std::optional<InverterIdentity> identity;
    std::optional<InverterSample>   sample;
    std::optional<InverterClock>    clock;
    PollStats stats;
};

// Written by the poller on the dispatcher thread, read by the HTTP thread.
class InverterState {
public:
    void set_connected(bool v) {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.connected = v;
        if (v) ++snap_.stats.reconnects;
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

    InverterSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return snap_;
    }

private:
    mutable std::mutex mtx_;
    InverterSnapshot snap_;
};
