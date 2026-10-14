#include "poller.hpp"

#include "dispatcher.hpp"
#include "history.hpp"
#include "inverter_state.hpp"
#include "modbus_rtu.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <ctime>
#include <iterator>

namespace {

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

std::string block_name(const RegisterBlock& b) {
    return "regs " + std::to_string(b.addr) + "+" + std::to_string(b.count);
}

}  // namespace

Poller::Poller(Dispatcher& dispatcher, const LoggerConfig& cfg,
               InverterState& state, History& history)
    : dispatcher_(dispatcher), cfg_(cfg), state_(state), history_(history),
      client_(dispatcher, cfg.host, cfg.port, cfg.serial,
              std::chrono::milliseconds(cfg.timeout_ms)) {
    spdlog::info("poller: logger {}:{} serial {} slave {}, every {} ms",
                 cfg_.host, cfg_.port, cfg_.serial, cfg_.slave_id,
                 cfg_.poll_period_ms);

    dispatcher_.watch(cycle_timer_.fd(), [this] {
        cycle_timer_.consume();
        start_cycle();
    });
    dispatcher_.watch(purge_timer_.fd(), [this] {
        purge_timer_.consume();
        history_.purge();
    });
    dispatcher_.watch(command_event_.fd(), [this] {
        command_event_.consume();
        pump();
    });
    purge_timer_.arm_periodic(kPurgeInterval);
    cycle_timer_.arm(std::chrono::milliseconds(0));
}

Poller::~Poller() {
    dispatcher_.unwatch(cycle_timer_.fd());
    dispatcher_.unwatch(purge_timer_.fd());
    dispatcher_.unwatch(command_event_.fd());
    client_.close();
}

bool Poller::submit(const InverterCommand& cmd, CommandDone done) {
    Command c;
    c.cmd  = &cmd;
    c.done = std::move(done);
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        if (queue_.size() >= kMaxQueuedCommands) return false;
        queue_.push_back(std::move(c));
    }
    command_event_.signal();
    return true;
}

void Poller::pump() {
    if (client_.busy()) return;
    if (!command_) {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        if (!queue_.empty()) {
            command_ = std::move(queue_.front());
            queue_.pop_front();
        }
    }
    if (!command_ && !cycle_active_) return;

    if (!client_.connected()) {
        client_.connect([this](bool ok, const std::string& e) { on_connected(ok, e); });
        return;
    }
    if (command_) command_step();
    else          read_block();
}

void Poller::on_connected(bool ok, const std::string& error) {
    if (!ok) {
        if (command_)     finish_command(false, error);
        if (cycle_active_) end_cycle(false, error);
        pump();
        return;
    }
    identity_known_ = false;
    state_.set_connected(true);
    if (cycle_active_ && blocks_[next_block_].addr != kIdentityBlock.addr)
        blocks_.insert(blocks_.begin() + next_block_, kIdentityBlock);
    pump();
}

// ---- poll cycle --------------------------------------------------------------

void Poller::start_cycle() {
    cycle_active_ = true;
    cycle_start_  = Clock::now();
    blocks_.clear();
    if (!identity_known_) blocks_.push_back(kIdentityBlock);
    blocks_.push_back(kClockBlock);
    blocks_.insert(blocks_.end(), std::begin(kLiveBlocks), std::end(kLiveBlocks));
    next_block_ = 0;
    bank_ = RegisterBank{};
    pump();
}

void Poller::read_block() {
    const RegisterBlock& b = blocks_[next_block_];
    client_.request(modbus::build_read_holding(cfg_.slave_id, b.addr, b.count),
                    [this](bool ok, std::vector<uint8_t> mb, const std::string& e) {
                        on_block(ok, std::move(mb), e);
                    });
}

void Poller::on_block(bool ok, std::vector<uint8_t> modbus,
                      const std::string& error) {
    const RegisterBlock b = blocks_[next_block_];
    std::vector<uint16_t> regs;
    std::string perr;
    if (!ok) {
        end_cycle(false, block_name(b) + ": " + error);
    } else if (!modbus::parse_read_holding(modbus, cfg_.slave_id, b.count, regs, perr)) {
        // The TCP stream is still in sync; only this answer was bad.
        end_cycle(false, block_name(b) + ": " + perr);
    } else {
        bank_.put(b.addr, regs);
        if (b.addr == kIdentityBlock.addr) publish_identity();
        if (++next_block_ == blocks_.size()) {
            publish_sample();
            end_cycle(true, {});
        }
    }
    pump();
}

void Poller::publish_identity() {
    InverterIdentity id = decode_identity(bank_);
    spdlog::info("poller: inverter serial {} device type {}",
                 id.serial, id.device_type);
    if (id.device_type != 5)
        spdlog::warn("poller: device type {} is not the SG04LP3 the register "
                     "map was written for; values may be wrong", id.device_type);
    state_.set_identity(std::move(id));
    identity_known_ = true;
}

void Poller::publish_sample() {
    InverterSample sample = decode_sample(bank_);
    std::optional<InverterClock> clock = decode_clock(bank_);
    const std::string day = clock ? clock->date() : local_date();
    const int64_t cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - cycle_start_).count();

    spdlog::debug("poller: pv={}W batt={}W soc={}% grid={}W load={}W ({} ms)",
                  sample.pv_power_w[0] + sample.pv_power_w[1],
                  sample.batt_power_w, sample.batt_soc_pct,
                  sample.grid_power_total_w, sample.load_power_total_w,
                  cycle_ms);

    history_.append(sample, day);
    state_.apply_cycle(std::move(sample), clock, cycle_ms);
}

void Poller::end_cycle(bool ok, const std::string& error) {
    cycle_active_ = false;
    if (ok) {
        if (failing_) spdlog::info("poller: inverter answering again");
        failing_ = false;
    } else {
        if (failing_) spdlog::debug("poller: {}", error);
        else          spdlog::warn("poller: {}", error);
        failing_ = true;
        state_.record_failure(error);
        if (!client_.connected() && state_.snapshot().connected)
            state_.set_connected(false);
    }

    // A lost connection waits reconnect_ms; anything else keeps the regular
    // cadence measured from the start of the cycle.
    const auto delay = std::chrono::milliseconds(
        client_.connected() ? cfg_.poll_period_ms : cfg_.reconnect_ms);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        cycle_start_ + delay - Clock::now());
    cycle_timer_.arm(std::max(left, std::chrono::milliseconds(0)));
}

// ---- commands ----------------------------------------------------------------

void Poller::command_step() {
    const Command& c = *command_;
    const auto frame = c.stage == Command::Stage::Write
        ? modbus::build_write_multiple(cfg_.slave_id, c.cmd->reg, {c.target})
        : modbus::build_read_holding(cfg_.slave_id, c.cmd->reg, 1);
    client_.request(frame, [this](bool ok, std::vector<uint8_t> mb,
                                  const std::string& e) {
        on_command_reply(ok, std::move(mb), e);
    });
}

void Poller::on_command_reply(bool ok, std::vector<uint8_t> modbus,
                              const std::string& error) {
    Command& c = *command_;
    const uint16_t reg = c.cmd->reg;
    std::vector<uint16_t> regs;
    std::string perr;

    if (!ok) {
        finish_command(false, error);
    } else if (c.stage == Command::Stage::Write) {
        if (!modbus::parse_write_multiple(modbus, cfg_.slave_id, reg, 1, perr))
            finish_command(false, "write reg " + std::to_string(reg) + ": " + perr);
        else
            c.stage = Command::Stage::Verify;
    } else if (!modbus::parse_read_holding(modbus, cfg_.slave_id, 1, regs, perr)) {
        finish_command(false, "read reg " + std::to_string(reg) + ": " + perr);
    } else if (c.stage == Command::Stage::Read) {
        c.result.before = regs[0];
        c.target = (regs[0] & ~c.cmd->mask) | c.cmd->bits;
        if (c.target == regs[0]) {
            c.result.after = regs[0];
            finish_command(true, {});
        } else {
            spdlog::warn("control: {}: writing reg {} 0x{:04x} -> 0x{:04x}",
                         c.cmd->name, reg, regs[0], c.target);
            c.stage = Command::Stage::Write;
        }
    } else {
        c.result.after = regs[0];
        if ((regs[0] & c.cmd->mask) == c.cmd->bits)
            finish_command(true, {});
        else
            finish_command(false, fmt::format("reg {} reads 0x{:04x} after the write",
                                              reg, regs[0]));
    }
    pump();
}

void Poller::finish_command(bool ok, std::string error) {
    Command c = std::move(*command_);
    command_.reset();
    c.result.ok    = ok;
    c.result.error = std::move(error);
    if (ok)
        spdlog::info("control: {}: done, reg {} = 0x{:04x}",
                     c.cmd->name, c.cmd->reg, c.result.after);
    else
        spdlog::warn("control: {}: failed: {}", c.cmd->name, c.result.error);
    if (c.done) c.done(c.result);
}
