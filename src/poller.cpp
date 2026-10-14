#include "poller.hpp"

#include "deye_registers.hpp"
#include "history.hpp"
#include "inverter_state.hpp"

#include <spdlog/spdlog.h>

#include <poll.h>

#include <ctime>

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPurgeInterval = std::chrono::hours(1);

// Fallback day key when the inverter clock cannot be decoded.
std::string local_date() {
    const time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &lt);
    return buf;
}

}  // namespace

Poller::Poller(const LoggerConfig& cfg, InverterState& state, History& history)
    : cfg_(cfg), state_(state), history_(history),
      client_(cfg.host, cfg.port, cfg.serial, cfg.slave_id,
              std::chrono::milliseconds(cfg.timeout_ms), stop_event_.fd()) {}

void Poller::sleep_until(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - Clock::now()).count();
    if (left <= 0) return;
    pollfd pfd{stop_event_.fd(), POLLIN, 0};
    ::poll(&pfd, 1, left);
}

bool Poller::poll_identity(std::string& error) {
    std::vector<uint16_t> regs;
    if (!client_.read_holding(kIdentityBlock.addr, kIdentityBlock.count,
                              regs, error))
        return false;
    RegisterBank bank;
    bank.put(kIdentityBlock.addr, regs);
    InverterIdentity id = decode_identity(bank);
    spdlog::info("poller: inverter serial {} device type {}",
                 id.serial, id.device_type);
    if (id.device_type != 5)
        spdlog::warn("poller: device type {} is not the SG04LP3 the register "
                     "map was written for; values may be wrong", id.device_type);
    state_.set_identity(std::move(id));
    identity_known_ = true;
    return true;
}

bool Poller::poll_cycle(std::string& error) {
    const auto t0 = Clock::now();
    RegisterBank bank;
    std::vector<uint16_t> regs;

    if (!client_.read_holding(kClockBlock.addr, kClockBlock.count, regs, error))
        return false;
    bank.put(kClockBlock.addr, regs);

    for (const auto& blk : kLiveBlocks) {
        if (!client_.read_holding(blk.addr, blk.count, regs, error))
            return false;
        bank.put(blk.addr, regs);
    }

    InverterSample sample = decode_sample(bank);
    std::optional<InverterClock> clock = decode_clock(bank);
    const std::string day = clock ? clock->date() : local_date();
    const int64_t cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - t0).count();

    spdlog::debug("poller: pv={}W batt={}W soc={}% grid={}W load={}W ({} ms)",
                  sample.pv_power_w[0] + sample.pv_power_w[1],
                  sample.batt_power_w, sample.batt_soc_pct,
                  sample.grid_power_total_w, sample.load_power_total_w,
                  cycle_ms);

    history_.append(sample, day);
    state_.apply_cycle(std::move(sample), clock, cycle_ms);
    return true;
}

void Poller::run() {
    spdlog::info("poller: logger {}:{} serial {} slave {}, every {} ms",
                 cfg_.host, cfg_.port, cfg_.serial, cfg_.slave_id,
                 cfg_.poll_period_ms);

    const auto period    = std::chrono::milliseconds(cfg_.poll_period_ms);
    const auto reconnect = std::chrono::milliseconds(cfg_.reconnect_ms);
    auto next_purge = Clock::now() + kPurgeInterval;
    // Log the first failure of a streak loudly, the repeats quietly, so an
    // inverter that is off for the night does not flood the log.
    bool failing = false;

    while (!stopping()) {
        const auto cycle_start = Clock::now();
        std::string error;

        bool ok = client_.connected() || client_.connect(error);
        if (ok && !state_.snapshot().connected) state_.set_connected(true);
        if (ok && !identity_known_) ok = poll_identity(error);
        if (ok) ok = poll_cycle(error);

        if (stopping()) break;

        if (ok) {
            if (failing) spdlog::info("poller: inverter answering again");
            failing = false;
        } else {
            if (failing) spdlog::debug("poller: {}", error);
            else         spdlog::warn("poller: {}", error);
            failing = true;
            state_.record_failure(error);
            if (!client_.connected()) {
                state_.set_connected(false);
                identity_known_ = false;
            }
        }

        if (Clock::now() >= next_purge) {
            history_.purge();
            next_purge = Clock::now() + kPurgeInterval;
        }

        // A lost connection waits reconnect_ms; anything else keeps the
        // regular cadence measured from the start of the cycle.
        sleep_until(cycle_start + (client_.connected() ? period : reconnect));
    }
    client_.close();
    spdlog::info("poller: stopped");
}
