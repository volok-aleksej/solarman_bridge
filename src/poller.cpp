#include "poller.hpp"

#include "dispatcher.hpp"
#include "history.hpp"
#include "inverter_state.hpp"
#include "modbus_rtu.hpp"

#include <spdlog/spdlog.h>

#include <ctime>

namespace {

// A batch is all there is in the queue: the reads of the cycle, the writes
// of a settings change and the reads of the settings.
constexpr size_t kMaxQueue = 32;

// Settings only change when someone sets them, so they are read this
// often. A change is worked out from the last read, without another one.
constexpr auto kSettingsRefresh = std::chrono::seconds(15);
constexpr const char* kSettings = "settings";

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

std::string block_name(const RegisterWrite& w) {
    return "regs " + std::to_string(w.addr) + "+" + std::to_string(w.values.size());
}

}  // namespace

Poller::Poller(Dispatcher& dispatcher, const InverterConfig& cfg,
               const InverterModel& model, InverterState& state, History& history,
               int history_id, std::chrono::milliseconds sample_period)
    : dispatcher_(dispatcher), cfg_(cfg), model_(model), state_(state), history_(history),
      history_id_(history_id), sample_period_(sample_period),
      client_(dispatcher, cfg.host, cfg.port, cfg.serial,
              std::chrono::milliseconds(cfg.timeout_ms),
              std::chrono::milliseconds(cfg.reconnect_ms), kMaxQueue),
      identity_block_(model.identity), blocks_(model.blocks) {
    spdlog::info("poller {}: {} via logger {}:{} serial {} slave {}, every {} ms",
                 cfg_.name, model_.name, cfg_.host, cfg_.port, cfg_.serial,
                 cfg_.slave_id, cfg_.poll_period_ms);
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
    if (up) {
        start_batch();
    } else {
        // A change is not kept for later: the settings it was worked out
        // from will be old by then.
        if (auto change = state_.take_settings()) {
            spdlog::warn("control {}: {}: not written: logger not connected",
                         cfg_.name, kSettings);
            if (change->done) change->done("logger not connected");
        }
        settings_read_at_.reset();   // read them as soon as the link is back
    }
    client_.tick();
}

// ---- batch -------------------------------------------------------------------

void Poller::start_batch() {
    // The previous batch still waits for answers: its requests are in the
    // queue, and a settings change waits in the state for the next one.
    if (batch_) return;

    batch_.emplace();
    batch_->start = Clock::now();
    batch_->readings.sample.ts = std::chrono::system_clock::now();
    batch_->left = 1;   // held while the requests are added, see the end

    // The poll cycle goes first, so the settings do not hold the live data
    // up. The identity does not change; it is read until it is known.
    if (!state_.has_identity()) read_block(identity_block_, true);
    for (RegisterBlock& b : blocks_) read_block(b, false);

    if (model_.has_settings()) {
        // A change, and then the settings as the inverter holds them after it.
        auto change = state_.take_settings();
        if (change) {
            batch_->change = std::move(*change);
            for (const RegisterWrite& w : batch_->change.writes) write_settings(w);
        }
        if (change || !settings_read_at_ ||
            batch_->start - *settings_read_at_ >= kSettingsRefresh) {
            // Stamped before the answer: a failed read is retried after the same pause.
            settings_read_at_ = batch_->start;
            batch_->settings.emplace();
            for (const RegisterBlock& b : model_.settings) read_settings(b);
        }
    }
    request_done();
}

void Poller::request(std::vector<uint8_t> frame, Answer done) {
    ++batch_->left;
    // Queued or not, every request ends here; the last one finishes the batch.
    const V5Client::Reply reply = [this, done = std::move(done)](
                                      bool ok, std::vector<uint8_t> mb, const std::string& e) {
        done(ok, mb, e);
        request_done();
    };
    if (!client_.submit(std::move(frame), reply, false)) reply(false, {}, "not queued");
}

void Poller::request_done() {
    if (--batch_->left == 0) finish_batch();
}

void Poller::read_block(RegisterBlock& b, bool identity) {
    request(modbus::build_read_holding(cfg_.slave_id, b.addr, b.count),
            [this, &b, identity](bool ok, const std::vector<uint8_t>& mb, const std::string& e) {
        batch_->cycle_end = Clock::now();
        std::vector<uint16_t> regs;
        std::string err = e;
        if (ok && modbus::parse_read_holding(mb, cfg_.slave_id, b.count, regs, err)) {
            if (b.on_block_done(regs, batch_->readings)) {
                if (identity && batch_->readings.identity)
                    publish_identity(*batch_->readings.identity);
                return;
            }
            err = "cannot be decoded";
        }
        if (batch_->error.empty()) batch_->error = block_name(b) + ": " + err;
    });
}

// `w` is a write of the batch's change and lives as long as the batch.
void Poller::write_settings(const RegisterWrite& w) {
    request(modbus::build_write_multiple(cfg_.slave_id, w.addr, w.values),
            [this, &w](bool ok, const std::vector<uint8_t>& mb, const std::string& e) {
        std::string err = e;
        if (ok && modbus::parse_write_multiple(mb, cfg_.slave_id, w.addr, w.values.size(), err)) {
            spdlog::info("control {}: {}: {} written", cfg_.name, kSettings, block_name(w));
            return;
        }
        err = block_name(w) + " not written: " + err;
        spdlog::warn("control {}: {}: {}", cfg_.name, kSettings, err);
        if (batch_->change_error.empty()) batch_->change_error = err;
    });
}

void Poller::read_settings(const RegisterBlock& b) {
    request(modbus::build_read_holding(cfg_.slave_id, b.addr, b.count),
            [this, &b](bool ok, const std::vector<uint8_t>& mb, const std::string& e) {
        std::vector<uint16_t> regs;
        std::string err = e;
        if (ok && modbus::parse_read_holding(mb, cfg_.slave_id, b.count, regs, err)) {
            if (batch_->settings) batch_->settings->put(b.addr, regs);
            return;
        }
        // Part of the settings is no settings: the state keeps the last full read.
        batch_->settings.reset();
        spdlog::debug("poller {}: {}: {}: {}", cfg_.name, kSettings, block_name(b), err);
    });
}

void Poller::publish_identity(InverterIdentity id) {
    spdlog::info("poller {}: inverter serial {} device type {}",
                 cfg_.name, id.serial, id.device_type);
    if (model_.device_type && id.device_type != model_.device_type)
        spdlog::warn("poller {}: device type {}, but model {} expects {}; values "
                     "may be wrong", cfg_.name, id.device_type, model_.name, model_.device_type);
    state_.set_identity(std::move(id));
}

void Poller::finish_batch() {
    // The batch and its readings end here, whatever comes of them.
    Batch b = std::move(*batch_);
    batch_.reset();

    const bool settings_read = b.settings.has_value();
    if (settings_read) state_.set_settings(std::move(*b.settings));
    // Whoever asked for the change is told what came of it, after the
    // state has the settings as the inverter holds them now.
    if (b.change.done) {
        if (b.change_error.empty() && !settings_read)
            b.change_error = "written, but the settings could not be read after it";
        b.change.done(b.change_error);
    }

    if (!b.error.empty()) {
        // A lost link is logged by the client; only cycle errors here.
        if (client_.connected()) {
            if (failing_) spdlog::debug("poller {}: {}", cfg_.name, b.error);
            else          spdlog::warn("poller {}: {}", cfg_.name, b.error);
            failing_ = true;
        }
        state_.record_failure(b.error);
        return;
    }
    if (failing_) spdlog::info("poller {}: inverter answering again", cfg_.name);
    failing_ = false;

    InverterSample& sample = b.readings.sample;
    const std::optional<InverterClock>& clock = b.readings.clock;
    const int64_t cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 b.cycle_end - b.start).count();
    spdlog::trace("poller {}: pv={}W batt={}W soc={}% grid={}W load={}W ({} ms)",
                  cfg_.name, sample.pv_power_w[0] + sample.pv_power_w[1],
                  sample.batt_power_w, sample.batt_soc_pct,
                  sample.grid_power_total_w, sample.load_power_total_w,
                  cycle_ms);

    // Half a poll period of slack, so cycle jitter does not push a stored
    // sample to the next cycle.
    const auto slack = std::chrono::milliseconds(cfg_.poll_period_ms / 2);
    if (!last_stored_ || b.start - *last_stored_ >= sample_period_ - slack) {
        history_.append(history_id_, sample, clock ? clock->date() : local_date());
        last_stored_ = b.start;
    }
    state_.apply_cycle(std::move(sample), clock, cycle_ms);
}
