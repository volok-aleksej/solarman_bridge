#include "poller.hpp"

#include "dispatcher.hpp"
#include "history.hpp"
#include "inverter_state.hpp"
#include "modbus_rtu.hpp"

#include <spdlog/spdlog.h>

#include <ctime>

namespace {

// Room for a few cycles; a queue that keeps growing means the poll period
// is shorter than the link needs, and the excess is dropped.
constexpr size_t kMaxQueue = 16;

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

CommandResult failure(std::string error) {
    CommandResult r;
    r.error = std::move(error);
    return r;
}

bool read_one(const std::vector<uint8_t>& modbus, uint8_t slave, uint16_t& value,
              std::string& error) {
    std::vector<uint16_t> regs;
    if (!modbus::parse_read_holding(modbus, slave, 1, regs, error)) return false;
    value = regs[0];
    return true;
}

}  // namespace

Poller::Poller(Dispatcher& dispatcher, const LoggerConfig& cfg,
               const InverterModel& model, InverterState& state, History& history,
               std::chrono::milliseconds sample_period)
    : dispatcher_(dispatcher), cfg_(cfg), model_(model), state_(state), history_(history),
      sample_period_(sample_period),
      client_(dispatcher, cfg.host, cfg.port, cfg.serial,
              std::chrono::milliseconds(cfg.timeout_ms),
              std::chrono::milliseconds(cfg.reconnect_ms), kMaxQueue) {
    spdlog::info("poller: {} via logger {}:{} serial {} slave {}, every {} ms",
                 model_.name, cfg_.host, cfg_.port, cfg_.serial, cfg_.slave_id,
                 cfg_.poll_period_ms);
    dispatcher_.watch(timer_.fd(), [this] {
        timer_.consume();
        on_tick();
    });
    timer_.arm_periodic(std::chrono::milliseconds(cfg_.poll_period_ms));
}

Poller::~Poller() {
    dispatcher_.unwatch(timer_.fd());
}

void Poller::on_tick() {
    const bool up = client_.connected();
    state_.set_connected(up, client_.last_error());
    for (const InverterCommand* cmd : state_.take_commands()) {
        if (up) start_command(*cmd);
        else    command_done(*cmd, failure("logger not connected"));
    }
    if (up) start_cycle();
    client_.tick();
}

// ---- poll cycle --------------------------------------------------------------

void Poller::start_cycle() {
    // The identity does not change; it is read until it is known.
    const bool with_identity = !state_.has_identity();

    auto c = std::make_shared<Cycle>();
    c->start = Clock::now();
    c->left  = model_.blocks.size() + with_identity;
    auto read = [&](const RegisterBlock& b, bool identity) {
        const bool queued = client_.submit(
            modbus::build_read_holding(cfg_.slave_id, b.addr, b.count),
            [this, c, b, identity](bool ok, std::vector<uint8_t> mb, const std::string& e) {
                on_block(*c, b, identity, ok, mb, e);
            },
            false);
        if (!queued) block_done(*c, block_name(b) + ": not queued");
    };
    if (with_identity) read(model_.identity, true);
    for (const RegisterBlock& b : model_.blocks) read(b, false);
}

void Poller::on_block(Cycle& c, const RegisterBlock& b, bool identity, bool ok,
                      const std::vector<uint8_t>& modbus, const std::string& error) {
    std::vector<uint16_t> regs;
    std::string perr;
    if (!ok) {
        block_done(c, block_name(b) + ": " + error);
    } else if (!modbus::parse_read_holding(modbus, cfg_.slave_id, b.count, regs, perr)) {
        block_done(c, block_name(b) + ": " + perr);
    } else {
        c.bank.put(b.addr, regs);
        if (identity) publish_identity(c.bank);
        block_done(c, {});
    }
}

void Poller::block_done(Cycle& c, std::string error) {
    if (c.error.empty()) c.error = std::move(error);
    if (--c.left == 0) finish_cycle(c);
}

void Poller::publish_identity(const RegisterBank& bank) {
    InverterIdentity id = model_.decode_identity(bank);
    spdlog::info("poller: inverter serial {} device type {}",
                 id.serial, id.device_type);
    if (model_.device_type && id.device_type != model_.device_type)
        spdlog::warn("poller: device type {}, but model {} expects {}; values "
                     "may be wrong", id.device_type, model_.name, model_.device_type);
    state_.set_identity(std::move(id));
}

void Poller::finish_cycle(Cycle& c) {
    if (!c.error.empty()) {
        // A lost link is logged by the client; only cycle errors here.
        if (client_.connected()) {
            if (failing_) spdlog::debug("poller: {}", c.error);
            else          spdlog::warn("poller: {}", c.error);
            failing_ = true;
        }
        state_.record_failure(c.error);
        return;
    }
    if (failing_) spdlog::info("poller: inverter answering again");
    failing_ = false;

    InverterSample sample = model_.decode_sample(c.bank);
    std::optional<InverterClock> clock = model_.decode_clock(c.bank);
    const int64_t cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - c.start).count();
    spdlog::trace("poller: pv={}W batt={}W soc={}% grid={}W load={}W ({} ms)",
                  sample.pv_power_w[0] + sample.pv_power_w[1],
                  sample.batt_power_w, sample.batt_soc_pct,
                  sample.grid_power_total_w, sample.load_power_total_w,
                  cycle_ms);

    // Half a poll period of slack, so cycle jitter does not push a stored
    // sample to the next cycle.
    const auto slack = std::chrono::milliseconds(cfg_.poll_period_ms / 2);
    if (!last_stored_ || c.start - *last_stored_ >= sample_period_ - slack) {
        history_.append(sample, clock ? clock->date() : local_date());
        last_stored_ = c.start;
    }
    state_.apply_cycle(std::move(sample), clock, cycle_ms);
}

// ---- commands: read, write, read back, all at the head of the queue ----------

void Poller::start_command(const InverterCommand& cmd) {
    const bool queued = client_.submit(
        modbus::build_read_holding(cfg_.slave_id, cmd.reg, 1),
        [this, &cmd](bool ok, std::vector<uint8_t> mb, const std::string& e) {
            on_command_read(cmd, ok, mb, e);
        },
        true);
    if (!queued) command_done(cmd, failure("read: not queued"));
}

void Poller::on_command_read(const InverterCommand& cmd, bool ok,
                             const std::vector<uint8_t>& modbus,
                             const std::string& error) {
    CommandResult r;
    std::string perr;
    if (!ok) {
        r.error = "read: " + error;
    } else if (!read_one(modbus, cfg_.slave_id, r.before, perr)) {
        r.error = "read: " + perr;
    } else {
        const uint16_t target = (r.before & ~cmd.mask) | cmd.bits;
        if (target == r.before) {
            r.ok    = true;
            r.after = r.before;
        } else {
            spdlog::warn("control: {}: writing reg {} 0x{:04x} -> 0x{:04x}",
                         cmd.name, cmd.reg, r.before, target);
            const bool queued = client_.submit(
                modbus::build_write_multiple(cfg_.slave_id, cmd.reg, {target}),
                [this, &cmd, r](bool ok2, std::vector<uint8_t> mb, const std::string& e) {
                    on_command_write(cmd, r, ok2, mb, e);
                },
                true);
            if (queued) return;
            r.error = "write: not queued";
        }
    }
    command_done(cmd, std::move(r));
}

void Poller::on_command_write(const InverterCommand& cmd, CommandResult r, bool ok,
                              const std::vector<uint8_t>& modbus,
                              const std::string& error) {
    std::string perr;
    if (!ok) {
        r.error = "write: " + error;
    } else if (!modbus::parse_write_multiple(modbus, cfg_.slave_id, cmd.reg, 1, perr)) {
        r.error = "write: " + perr;
    } else {
        const bool queued = client_.submit(
            modbus::build_read_holding(cfg_.slave_id, cmd.reg, 1),
            [this, &cmd, r](bool ok2, std::vector<uint8_t> mb, const std::string& e) {
                on_command_verify(cmd, r, ok2, mb, e);
            },
            true);
        if (queued) return;
        r.error = "verify: not queued";
    }
    command_done(cmd, std::move(r));
}

void Poller::on_command_verify(const InverterCommand& cmd, CommandResult r, bool ok,
                               const std::vector<uint8_t>& modbus,
                               const std::string& error) {
    std::string perr;
    if (!ok) {
        r.error = "verify: " + error;
    } else if (!read_one(modbus, cfg_.slave_id, r.after, perr)) {
        r.error = "verify: " + perr;
    } else if ((r.after & cmd.mask) != cmd.bits) {
        r.error = fmt::format("reg {} reads 0x{:04x} after the write", cmd.reg, r.after);
    } else {
        r.ok = true;
    }
    command_done(cmd, std::move(r));
}

void Poller::command_done(const InverterCommand& cmd, CommandResult r) {
    if (r.ok)
        spdlog::info("control: {}: done, reg {} = 0x{:04x}", cmd.name, cmd.reg, r.after);
    else
        spdlog::warn("control: {}: failed: {}", cmd.name, r.error);
    state_.set_command(cmd.name, std::move(r));
}
