#pragma once

#include "inverter_data.hpp"
#include "inverter_settings.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct PollStats {
    uint64_t cycles_ok     = 0;
    uint64_t cycles_failed = 0;
    uint64_t reconnects    = 0;
    int64_t  last_cycle_ms = 0;   // duration of the last successful cycle
    std::string last_error;
    std::chrono::system_clock::time_point last_error_at{};
};

// A settings change the HTTP side leaves for the poller.
struct SettingsChange {
    std::vector<RegisterWrite> writes;
    // Called by the poller, on its thread, when the change is over: with ""
    // when all of it is written and the settings are read again, otherwise
    // with the reason.
    std::function<void(const std::string& error)> done;
};

// Snapshot of everything the poller has learned about the inverter.
struct InverterSnapshot {
    bool connected = false;
    std::optional<InverterIdentity> identity;
    std::optional<InverterSample>   sample;
    std::optional<InverterClock>    clock;
    // The model's settings registers as last read from the inverter.
    std::optional<RegisterBank>     settings;
    std::chrono::system_clock::time_point settings_at{};
    PollStats stats;
};

// Written by the poller on the dispatcher thread, read by the HTTP thread.
// The HTTP thread also leaves settings changes here for the poller to write.
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

    void set_settings(RegisterBank bank) {
        std::lock_guard<std::mutex> lk(mtx_);
        snap_.settings    = std::move(bank);
        snap_.settings_at = std::chrono::system_clock::now();
        settings_changing_ = false;
    }

    // HTTP thread: registers to write. False while an earlier change is
    // not written and read back yet.
    bool request_settings(SettingsChange change) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pending_settings_ || settings_changing_) return false;
        pending_settings_ = std::move(change);
        return true;
    }

    // Poller: the change requested since the last call, if any.
    std::optional<SettingsChange> take_settings() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pending_settings_) settings_changing_ = true;
        return std::exchange(pending_settings_, std::nullopt);
    }

    InverterSnapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return snap_;
    }

private:
    mutable std::mutex mtx_;
    InverterSnapshot snap_;
    std::optional<SettingsChange> pending_settings_;
    bool settings_changing_ = false;   // taken by the poller, not read again yet
};
