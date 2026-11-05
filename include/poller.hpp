#pragma once

#include "config.hpp"
#include "inverter_model.hpp"
#include "timer.hpp"
#include "v5_client.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Dispatcher;
class History;
class InverterState;

// Polls one inverter through its logger and writes its settings.
//
// One timer, every poll_period_ms: it drives the V5 client and, when the
// link is up, puts together one batch of requests for the inverter and
// queues it there. Results land in InverterState and History (one sample
// per `sample_period`); nothing waits for them. Dispatcher thread only.
class Poller {
public:
    Poller(Dispatcher& dispatcher, const InverterConfig& cfg,
           const InverterModel& model, InverterState& state, History& history,
           int history_id, std::chrono::milliseconds sample_period);
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

private:
    using Clock = std::chrono::steady_clock;

    // Everything the poller asks the inverter at one tick, sent one request
    // after another:
    //  - the reads of the poll cycle, every answer decoded by its block;
    //  - the writes of a settings change, when the HTTP side has left one
    //    in InverterState. They are sent as they are, nothing is read first;
    //  - the reads of the settings registers, after a change and every now
    //    and then.
    // Made in start_batch() and gone in finish_batch(), when the last
    // request is answered or failed; the results go to the state there. A
    // tick that finds a batch still waiting starts no other, so the
    // requests of two batches never mix.
    struct Batch {
        Clock::time_point start;
        Clock::time_point cycle_end;            // last answer to a read of the cycle
        size_t            left = 0;             // requests not answered yet
        InverterReadings  readings;             // what the blocks of the cycle decode
        std::string       error;                // first failure of the cycle, empty if none
        std::optional<RegisterBank> settings;   // the settings registers, when the batch
                                                // reads them; dropped when a read fails
    };

    // The answer to one request of the batch, or why there is none.
    using Answer = std::function<void(bool ok, const std::vector<uint8_t>& modbus,
                                      const std::string& error)>;

    void on_tick();
    void start_batch();
    // Adds a request to the batch; `done` gets its answer.
    void request(std::vector<uint8_t> frame, Answer done);
    void request_done();
    void finish_batch();

    // The three kinds of request a batch is made of.
    void read_block(RegisterBlock& b, bool identity);
    void write_settings(const RegisterWrite& w);
    void read_settings(const RegisterBlock& b);

    void publish_identity(InverterIdentity id);

    Dispatcher&               dispatcher_;
    InverterConfig            cfg_;
    const InverterModel&      model_;
    InverterState&            state_;
    History&                  history_;
    int                       history_id_;
    std::chrono::milliseconds sample_period_;
    V5Client                  client_;
    Timer                     timer_;

    RegisterBlock              identity_block_;
    std::vector<RegisterBlock> blocks_;
    std::optional<Batch>       batch_;

    // Cycles keep failing; only the first one is logged as a warning.
    bool failing_        = false;
    std::optional<Clock::time_point> last_stored_;

    // When the settings were last asked for; none: with the next batch.
    std::optional<Clock::time_point> settings_read_at_;
};
