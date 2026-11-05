#include "dispatcher.hpp"
#include "history.hpp"
#include "inverter_model.hpp"
#include "inverter_settings.hpp"
#include "inverter_state.hpp"
#include "modbus_rtu.hpp"
#include "poller.hpp"
#include "solarman_v5.hpp"
#include "v5_client.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kSerial = 3581987480;

// V5 response carrying `modbus`, answering a request with sequence `seq`.
std::vector<uint8_t> v5_response(uint16_t control, uint8_t seq,
                                 const std::vector<uint8_t>& modbus) {
    std::vector<uint8_t> f = {v5::kStart, 0, 0,
                              uint8_t(control & 0xFF), uint8_t(control >> 8),
                              seq, 0};
    for (int i = 0; i < 4; ++i) f.push_back((kSerial >> (8 * i)) & 0xFF);
    f.push_back(0x02);                  // frame type
    f.push_back(0x01);                  // status
    f.insert(f.end(), 12, 0);           // three u32 time fields
    f.insert(f.end(), modbus.begin(), modbus.end());
    const size_t payload = f.size() - v5::kHeaderLen;
    f[1] = payload & 0xFF;
    f[2] = payload >> 8;
    f.push_back(0);
    f.push_back(v5::kEnd);
    f[f.size() - 2] = v5::checksum(f.data(), f.size());
    return f;
}

std::vector<uint8_t> modbus_regs(const std::vector<uint16_t>& regs) {
    std::vector<uint8_t> f = {1, modbus::kReadHoldingRegisters,
                              uint8_t(regs.size() * 2)};
    for (uint16_t r : regs) { f.push_back(r >> 8); f.push_back(r & 0xFF); }
    const uint16_t crc = modbus::crc16(f.data(), f.size());
    f.push_back(crc & 0xFF);
    f.push_back(crc >> 8);
    return f;
}

// A logger on 127.0.0.1 driven by the same dispatcher. `on_request` gets
// each complete request frame and the accepted socket to answer on.
class FakeLogger {
public:
    using OnRequest = std::function<void(int fd, const std::vector<uint8_t>&)>;

    FakeLogger(Dispatcher& d, OnRequest on_request)
        : d_(d), on_request_(std::move(on_request)) {
        lfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(lfd_, 4);
        socklen_t len = sizeof(a);
        ::getsockname(lfd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        d_.watch(lfd_, [this] { on_accept(); });
    }
    ~FakeLogger() {
        drop_client();
        d_.unwatch(lfd_);
        ::close(lfd_);
    }

    uint16_t port() const { return port_; }
    int accepted() const { return accepted_; }
    void drop_client() {
        if (cfd_ < 0) return;
        d_.unwatch(cfd_);
        ::close(cfd_);
        cfd_ = -1;
    }

private:
    void on_accept() {
        drop_client();
        cfd_ = ::accept4(lfd_, nullptr, nullptr, SOCK_CLOEXEC);
        ++accepted_;
        d_.watch(cfd_, [this] { on_data(); });
    }
    void on_data() {
        uint8_t buf[512];
        const ssize_t n = ::recv(cfd_, buf, sizeof(buf), 0);
        if (n <= 0) { drop_client(); return; }
        rx_.insert(rx_.end(), buf, buf + n);
        size_t junk = 0;
        while (const size_t len = v5::frame_length(rx_, junk)) {
            std::vector<uint8_t> frame(rx_.begin(), rx_.begin() + len);
            rx_.erase(rx_.begin(), rx_.begin() + len);
            on_request_(cfd_, frame);
        }
    }

    Dispatcher& d_;
    OnRequest   on_request_;
    int         lfd_ = -1;
    int         cfd_ = -1;
    uint16_t    port_ = 0;
    int         accepted_ = 0;
    std::vector<uint8_t> rx_;
};

void send_bytes(int fd, const std::vector<uint8_t>& b) {
    ASSERT_EQ(::send(fd, b.data(), b.size(), MSG_NOSIGNAL), ssize_t(b.size()));
}

const std::vector<uint8_t> kReadClock = modbus::build_read_holding(1, 62, 3);

struct Result {
    bool done = false;
    bool ok   = false;
    std::vector<uint8_t> modbus;
    std::string error;
};

// Drives `c` like the poller does: tick() every 10 ms.
struct Harness {
    Dispatcher d;
    std::unique_ptr<V5Client> c;
    std::function<bool()> until;
    void start(uint16_t port, int timeout_ms = 1000, size_t max_queue = 16) {
        c = std::make_unique<V5Client>(d, "127.0.0.1", port, kSerial,
                                       std::chrono::milliseconds(timeout_ms),
                                       std::chrono::milliseconds(50), max_queue);
        d.add_timer(10, [this] {
            c->tick();
            if (until && until()) d.stop();
        });
    }
    // Runs the loop until `pred` holds, checked every 10 ms.
    void run_until(std::function<bool()> pred) {
        until = std::move(pred);
        d.run();
        until = nullptr;
    }
    // Connects, sends one request, runs until it is answered.
    Result round_trip(const std::vector<uint8_t>& frame) {
        run_until([this] { return c->connected(); });
        Result r;
        EXPECT_TRUE(c->submit(frame, [&](bool ok, std::vector<uint8_t> mb, const std::string& e) {
            r = {true, ok, std::move(mb), e};
        }, false));
        run_until([&] { return r.done; });
        return r;
    }
};

}  // namespace

TEST(V5Client, SkipsCounterAndStaleFramesThenReturnsReply) {
    Harness h;
    const auto regs = modbus_regs({0x1a0a, 0x0e10, 0x2715});
    FakeLogger logger(h.d, [&](int fd, const std::vector<uint8_t>& req) {
        const uint8_t seq = req[5];
        // Modbus frame of the request starts after header + 15 bytes.
        EXPECT_EQ(std::vector<uint8_t>(req.begin() + 26, req.end() - 2), kReadClock);
        send_bytes(fd, v5_response(v5::kControlCounter, seq, {}));
        send_bytes(fd, v5_response(v5::kControlResponse, seq - 1, regs));
        send_bytes(fd, v5_response(v5::kControlResponse, seq, regs));
    });
    h.start(logger.port());

    const Result r = h.round_trip(kReadClock);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.modbus, regs);
    EXPECT_TRUE(h.c->connected());
}

TEST(V5Client, TimesOutAndCloses) {
    Harness h;
    FakeLogger logger(h.d, [](int, const std::vector<uint8_t>&) {});  // never answers
    h.start(logger.port(), 100);

    const Result r = h.round_trip(kReadClock);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "receive: timeout");
    EXPECT_FALSE(h.c->connected());
}

TEST(V5Client, ReportsPeerClose) {
    Harness h;
    FakeLogger* lp = nullptr;
    FakeLogger logger(h.d, [&](int, const std::vector<uint8_t>&) { lp->drop_client(); });
    lp = &logger;
    h.start(logger.port());

    const Result r = h.round_trip(kReadClock);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "receive: logger closed the connection");
    EXPECT_FALSE(h.c->connected());
}

TEST(V5Client, RefusesRequestsWithoutALink) {
    Harness h;
    uint16_t port;
    {
        // A port that was just free: nothing listens there now.
        FakeLogger tmp(h.d, [](int, const std::vector<uint8_t>&) {});
        port = tmp.port();
    }
    h.start(port);
    auto never = [](bool, std::vector<uint8_t>, const std::string&) { ADD_FAILURE(); };
    EXPECT_FALSE(h.c->submit(kReadClock, never, false));

    int ticks = 0;
    h.run_until([&] { return ++ticks == 20; });   // several refused connects
    EXPECT_FALSE(h.c->connected());
    EXPECT_FALSE(h.c->last_error().empty());
    EXPECT_FALSE(h.c->submit(kReadClock, never, false));
}

TEST(V5Client, LinkLossFailsEveryQueuedRequest) {
    Harness h;
    FakeLogger* lp = nullptr;
    FakeLogger logger(h.d, [&](int, const std::vector<uint8_t>&) { lp->drop_client(); });
    lp = &logger;
    h.start(logger.port());
    h.run_until([&] { return h.c->connected(); });

    std::vector<std::string> errors;
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(h.c->submit(kReadClock, [&](bool ok, std::vector<uint8_t>, const std::string& e) {
            EXPECT_FALSE(ok);
            errors.push_back(e);
        }, false));
    h.run_until([&] { return errors.size() == 3; });
    for (const auto& e : errors) EXPECT_EQ(e, "receive: logger closed the connection");
}

TEST(V5Client, UrgentRequestsJumpTheQueue) {
    Harness h;
    std::vector<uint16_t> seen;   // register addresses in the order sent
    FakeLogger logger(h.d, [&](int fd, const std::vector<uint8_t>& req) {
        seen.push_back((req[28] << 8) | req[29]);
        send_bytes(fd, v5_response(v5::kControlResponse, req[5], modbus_regs({0})));
    });
    h.start(logger.port());
    h.run_until([&] { return h.c->connected(); });

    int answered = 0;
    auto count = [&](bool, std::vector<uint8_t>, const std::string&) { ++answered; };
    // All queued before the next tick sends the first one.
    ASSERT_TRUE(h.c->submit(modbus::build_read_holding(1, 10, 1), count, false));
    ASSERT_TRUE(h.c->submit(modbus::build_read_holding(1, 20, 1), count, false));
    ASSERT_TRUE(h.c->submit(modbus::build_read_holding(1, 80, 1), count, true));
    h.run_until([&] { return answered == 3; });
    EXPECT_EQ(seen, (std::vector<uint16_t>{80, 10, 20}));
}

TEST(V5Client, DropsRequestsBeyondTheLimit) {
    Harness h;
    FakeLogger logger(h.d, [](int, const std::vector<uint8_t>&) {});
    h.start(logger.port(), 1000, 2);
    h.run_until([&] { return h.c->connected(); });

    auto ignore = [](bool, std::vector<uint8_t>, const std::string&) {};
    EXPECT_TRUE (h.c->submit(kReadClock, ignore, false));
    EXPECT_TRUE (h.c->submit(kReadClock, ignore, false));
    EXPECT_FALSE(h.c->submit(kReadClock, ignore, false));
    EXPECT_FALSE(h.c->submit(kReadClock, ignore, true));   // writes too
}

TEST(Poller, RunsFullCyclesAndStoresSamples) {
    Dispatcher d;
    int requests = 0;
    FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) {
        ++requests;
        const uint16_t addr = (req[28] << 8) | req[29];
        const uint16_t qty  = (req[30] << 8) | req[31];
        std::vector<uint16_t> regs(qty, 0);
        if (addr == 0)   regs[0] = 5;           // device type SG04LP3
        if (addr == 62)  regs = {0x1a0a, 0x0e10, 0x2715};
        if (addr == 586) regs[588 - 586] = 77;  // SOC
        send_bytes(fd, v5_response(v5::kControlResponse, req[5], modbus_regs(regs)));
    });

    const std::string db = "/tmp/solarman_poller_test_" + std::to_string(getpid()) + ".sqlite3";
    InverterState state;
    {
        History history(db, 30);
        InverterConfig cfg;
        cfg.name = "main";
        cfg.host = "127.0.0.1";
        cfg.port = logger.port();
        cfg.serial = kSerial;
        cfg.poll_period_ms = 50;
        Poller poller(d, cfg, *find_model("deye_sg04lp3"), state, history,
                      history.inverter_id("main"), std::chrono::milliseconds(0));
        d.add_timer(10, [&] {
            if (state.snapshot().stats.cycles_ok >= 3) d.stop();
        });
        d.run();

        const auto now  = std::chrono::system_clock::now();
        const auto rows = history.range(history.inverter_id("main"), now - std::chrono::minutes(1),
                                        now + std::chrono::minutes(1), 0);
        EXPECT_GE(rows.size(), 3u);
        const auto days = history.daily(history.inverter_id("main"), 2026, 10);
        ASSERT_EQ(days.size(), 1u);
        EXPECT_EQ(days[0].date, "2026-10-14");  // from the inverter clock
    }
    for (const char* sfx : {"", "-wal", "-shm"}) std::remove((db + sfx).c_str());

    const auto snap = state.snapshot();
    EXPECT_TRUE(snap.connected);
    ASSERT_TRUE(snap.identity);
    EXPECT_EQ(snap.identity->device_type, 5);
    ASSERT_TRUE(snap.sample);
    EXPECT_EQ(snap.sample->batt_soc_pct, 77);
    EXPECT_STREQ(snap.sample->run_state_name, "standby");
    EXPECT_FALSE(snap.sample->running);
    EXPECT_EQ(snap.stats.cycles_failed, 0u);
    // Identity once, then clock + 2 live blocks per cycle, on one connection.
    EXPECT_EQ(logger.accepted(), 1);
    EXPECT_GE(requests, 1 + 3 * 3);
}

namespace {

// A fake inverter behind the fake logger: zeroed registers except the
// identity, the clock and the settings 98..177, which also take 0x10 writes.
struct FakeInverter {
    std::vector<uint16_t> settings = std::vector<uint16_t>(80, 0);   // from 98
    bool     reject_writes = false;
    bool     ignore_writes = false;   // acknowledged, not stored
    int      writes = 0;
    int      reads  = 0;
    int      settings_reads = 0;      // reads of the settings block
    int      stray_writes = 0;        // registers written outside the settings
    std::vector<uint16_t> written;    // first register of every write
    std::vector<std::string> requests;   // in the order they came: "r500", "w154"

    FakeInverter() {
        setting(111) = 1;             // battery managed by SOC
        setting(115) = 10;            // shutdown, restart, low battery
        setting(116) = 20;
        setting(117) = 15;
        for (int i = 0; i < 6; ++i) {
            setting(148 + i) = 100 + 400 * i;   // window starts
            setting(154 + i) = 6000;            // power
            setting(166 + i) = 50;              // SOC
            setting(172 + i) = 3;               // charge from grid and generator
        }
    }
    uint16_t& setting(uint16_t reg) { return settings[reg - 98]; }
    bool is_setting(uint16_t reg) const { return reg >= 98 && reg < 98 + settings.size(); }

    void serve(int fd, const std::vector<uint8_t>& req) {
        const uint8_t  func = req[27];
        const uint16_t addr = (req[28] << 8) | req[29];
        const uint16_t qty  = (req[30] << 8) | req[31];
        std::vector<uint8_t> mb;
        requests.push_back((func == modbus::kWriteMultipleRegisters ? "w" : "r") +
                           std::to_string(addr));
        if (func == modbus::kWriteMultipleRegisters) {
            ++writes;
            written.push_back(addr);
            if (reject_writes) {
                mb = {1, 0x90, 0x02};
            } else {
                for (uint16_t i = 0; i < qty && !ignore_writes; ++i) {
                    const uint16_t v = (req[33 + 2 * i] << 8) | req[34 + 2 * i];
                    if (is_setting(addr + i)) setting(addr + i) = v;
                    else                      ++stray_writes;
                }
                mb = {1, 0x10, req[28], req[29], req[30], req[31]};
            }
            const uint16_t crc = modbus::crc16(mb.data(), mb.size());
            mb.push_back(crc & 0xFF);
            mb.push_back(crc >> 8);
        } else {
            ++reads;
            std::vector<uint16_t> regs(qty, 0);
            if (addr == 0)  regs[0] = 5;
            if (addr == 20) regs = {0x49f0, 2};                      // 15 kW
            if (addr == 62) regs = {0x1a0a, 0x0e10, 0x2715};
            if (addr == 98) ++settings_reads;
            for (uint16_t i = 0; i < qty; ++i)
                if (is_setting(addr + i)) regs[i] = setting(addr + i);
            mb = modbus_regs(regs);
        }
        send_bytes(fd, v5_response(v5::kControlResponse, req[5], mb));
    }
};

// What a settings change comes to is not reported anywhere but the log, so
// the tests look at what the inverter holds and at the settings as the
// poller has read them.
class PollerSettingsTest : public ::testing::Test {
protected:
    using Over = std::function<bool(const InverterSnapshot&)>;

    void SetUp() override {
        db_ = "/tmp/solarman_settings_test_" + std::to_string(getpid()) + ".sqlite3";
    }
    void TearDown() override {
        for (const char* sfx : {"", "-wal", "-shm"}) std::remove((db_ + sfx).c_str());
    }

    // Runs the poller, requests `writes` once the settings are read (none:
    // nothing is requested), and goes on until `over` holds and then for
    // `linger_ms` more, to see what else the change does. False when `over`
    // never came. `state` is left as the poller made it.
    bool change(FakeInverter& inv, std::vector<RegisterWrite> writes, InverterState& state,
                Over over, int linger_ms = 0) {
        Dispatcher d;
        FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) {
            inv.serve(fd, req);
        });
        History history(db_, 30);
        InverterConfig cfg;
        cfg.name = "main";
        cfg.host = "127.0.0.1";
        cfg.port = logger.port();
        cfg.serial = kSerial;
        cfg.poll_period_ms = 50;
        Poller poller(d, cfg, *find_model("deye_sg04lp3"), state, history,
                      history.inverter_id("main"), std::chrono::milliseconds(0));

        bool submitted = false, came = false;
        int ticks = 0, linger = -1;       // of 10 ms
        d.add_timer(10, [&] {
            const auto snap = state.snapshot();
            if (!submitted) {
                if (!snap.settings) return;
                submitted = true;
                if (!writes.empty()) {
                    EXPECT_TRUE(state.request_settings(writes));
                    EXPECT_FALSE(state.request_settings(writes));   // one at a time
                }
            } else if (linger < 0) {
                if (over(snap)) { came = true; linger = linger_ms / 10; }
                else if (++ticks > 300) d.stop();                // 3 s: it never came
            }
            if (linger >= 0 && linger-- == 0) d.stop();
        });
        d.run();
        EXPECT_EQ(inv.stray_writes, 0);
        return came;
    }

    std::string db_;
};

// Three poll periods: time enough for a change to do whatever it does.
constexpr int kSettle = 150;

}  // namespace

TEST_F(PollerSettingsTest, SettingsAreReadWithoutBeingAsked) {
    FakeInverter inv;
    InverterState state;
    ASSERT_TRUE(change(inv, {}, state, [](const InverterSnapshot&) { return true; }, kSettle));
    EXPECT_EQ(inv.writes, 0);
    EXPECT_EQ(inv.settings_reads, 1);             // and not again before the refresh is due
    const auto snap = state.snapshot();
    ASSERT_TRUE(snap.settings);
    EXPECT_EQ(snap.settings->u16(148), 100);
    EXPECT_EQ(snap.settings->u16(159), 6000);
    EXPECT_EQ(snap.settings->u16(171), 50);
    EXPECT_EQ(snap.settings->u32(20), 150000u);   // rated power, read along
}

// Power and SOC of all six windows: two runs of six registers, two writes,
// nothing read before them and the settings read once after.
TEST_F(PollerSettingsTest, SettingsChangeWritesEveryRunAndReadsAfter) {
    FakeInverter inv;
    InverterState state;
    const std::vector<RegisterWrite> writes{
        {154, std::vector<uint16_t>(6, 4000)},
        {166, std::vector<uint16_t>(6, 30)},
    };
    // Over when the page can see it: the settings read after hold the new values.
    ASSERT_TRUE(change(inv, writes, state, [](const InverterSnapshot& snap) {
        return snap.settings->u16(156) == 4000 && snap.settings->u16(170) == 30;
    }, kSettle));
    EXPECT_EQ(inv.written, (std::vector<uint16_t>{154, 166}));
    EXPECT_EQ(inv.settings_reads, 2);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(inv.setting(154 + i), 4000);
        EXPECT_EQ(inv.setting(166 + i), 30);
        EXPECT_EQ(inv.setting(148 + i), 100 + 400 * i);   // untouched
        EXPECT_EQ(inv.setting(172 + i), 3);
    }
}

// A change travels in one batch with the poll cycle: the reads of the
// cycle, the writes, then the settings as the inverter holds them now.
TEST_F(PollerSettingsTest, SettingsChangeGoesInOneBatchAfterTheCycle) {
    FakeInverter inv;
    InverterState state;
    const std::vector<RegisterWrite> writes{
        {154, std::vector<uint16_t>(6, 4000)},
        {108, {60}},
    };
    ASSERT_TRUE(change(inv, writes, state, [](const InverterSnapshot& snap) {
        return snap.settings->u16(108) == 60;
    }));
    const auto w = std::find(inv.requests.begin(), inv.requests.end(), "w154");
    ASSERT_GE(w - inv.requests.begin(), 3);
    ASSERT_GE(inv.requests.end() - w, 4);
    EXPECT_EQ(std::vector<std::string>(w - 3, w + 4),
              (std::vector<std::string>{"r62", "r500", "r586", "w154", "w108", "r20", "r98"}));
}

// The inverter refuses the writes: every one is tried once, and the
// settings are read again so the page shows what the inverter still holds.
TEST_F(PollerSettingsTest, SettingsWritesRefusedByTheInverterAreNotRepeated) {
    FakeInverter inv;
    inv.reject_writes = true;
    InverterState state;
    const std::vector<RegisterWrite> writes{
        {154, std::vector<uint16_t>(6, 4000)},
        {108, {60}},
    };
    ASSERT_TRUE(change(inv, writes, state, [&](const InverterSnapshot&) {
        return inv.writes >= 2 && inv.settings_reads >= 2;   // refused, then read anew
    }, kSettle));
    EXPECT_EQ(inv.written, (std::vector<uint16_t>{154, 108}));
    EXPECT_EQ(inv.setting(154), 6000);
    EXPECT_EQ(state.snapshot().settings->u16(154), 6000);
}

// The inverter acknowledges the write and keeps the old value: the read
// after it shows that, and that is what the page gets.
TEST_F(PollerSettingsTest, SettingsChangeTheInverterDidNotTakeShowsTheOldValue) {
    FakeInverter inv;
    inv.ignore_writes = true;
    inv.setting(108) = 90;
    InverterState state;
    ASSERT_TRUE(change(inv, {{108, {60}}}, state, [&](const InverterSnapshot&) {
        return inv.writes >= 1 && inv.settings_reads >= 2;
    }, kSettle));
    EXPECT_EQ(inv.written, (std::vector<uint16_t>{108}));
    EXPECT_EQ(state.snapshot().settings->u16(108), 90);
}

// A change is worked out from the settings as last read, so the next one is
// not taken until they have been read again after the writes of this one.
TEST_F(PollerSettingsTest, NextSettingsChangeWaitsForTheReadAfterThisOne) {
    FakeInverter inv;
    inv.setting(108) = 90;
    Dispatcher d;
    FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) { inv.serve(fd, req); });
    InverterState state;
    History history(db_, 30);
    InverterConfig cfg;
    cfg.name = "main";
    cfg.host = "127.0.0.1";
    cfg.port = logger.port();
    cfg.serial = kSerial;
    cfg.poll_period_ms = 50;
    Poller poller(d, cfg, *find_model("deye_sg04lp3"), state, history,
                  history.inverter_id("main"), std::chrono::milliseconds(0));

    int step = 0;             // 0 wait for the settings, 1 first change, 2 second
    int refused = 0, ticks = 0;
    d.add_timer(10, [&] {
        const auto snap = state.snapshot();
        if (step == 0 && snap.settings) {
            step = 1;
            EXPECT_TRUE(state.request_settings({{108, {60}}}));
        } else if (step == 1) {
            // Tried on every tick: taken only once the first one shows.
            if (state.request_settings({{109, {120}}})) {
                EXPECT_EQ(snap.settings->u16(108), 60);
                step = 2;
            } else {
                ++refused;
            }
        } else if (step == 2 && snap.settings->u16(109) == 120) {
            d.stop();
        }
        if (++ticks > 300) d.stop();
    });
    d.run();

    EXPECT_EQ(step, 2);
    EXPECT_GT(refused, 0);
    EXPECT_EQ(inv.written, (std::vector<uint16_t>{108, 109}));
    EXPECT_EQ(inv.setting(108), 60);
    EXPECT_EQ(inv.setting(109), 120);
    EXPECT_EQ(inv.stray_writes, 0);
}

// A logger slower than the poll period: a tick that finds the cycle still
// waiting starts no other, so cycles neither pile up nor mix their readings.
TEST(Poller, SlowLoggerDoesNotOverlapCycles) {
    Dispatcher d;
    FakeInverter inv;
    std::vector<std::pair<int, std::vector<uint8_t>>> waiting;   // requests not answered yet
    size_t most_waiting = 0;
    FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) {
        waiting.emplace_back(fd, req);
        most_waiting = std::max(most_waiting, waiting.size());
    });
    // One answer every 30 ms against a 20 ms poll period.
    d.add_timer(30, [&] {
        if (waiting.empty()) return;
        const auto [fd, req] = waiting.front();
        waiting.erase(waiting.begin());
        inv.serve(fd, req);
    });

    const std::string db = "/tmp/solarman_slow_test_" + std::to_string(getpid()) + ".sqlite3";
    InverterState state;
    {
        History history(db, 30);
        InverterConfig cfg;
        cfg.name = "main";
        cfg.host = "127.0.0.1";
        cfg.port = logger.port();
        cfg.serial = kSerial;
        cfg.poll_period_ms = 20;
        Poller poller(d, cfg, *find_model("deye_sg04lp3"), state, history,
                      history.inverter_id("main"), std::chrono::seconds(10));
        d.add_timer(10, [&] {
            if (state.snapshot().stats.cycles_ok >= 4) d.stop();
        });
        d.run();
    }
    for (const char* sfx : {"", "-wal", "-shm"}) std::remove((db + sfx).c_str());

    const auto snap = state.snapshot();
    EXPECT_GE(snap.stats.cycles_ok, 4u);
    EXPECT_EQ(snap.stats.cycles_failed, 0u);
    // Never more in the logger's hands than one cycle (identity + 3 blocks)
    // and one read of the settings (2 blocks, one after another).
    EXPECT_LE(most_waiting, 5u);
    ASSERT_TRUE(snap.sample);
    EXPECT_STREQ(snap.sample->run_state_name, "standby");
}

TEST(Poller, StoresOneSamplePerSamplePeriod) {
    Dispatcher d;
    FakeInverter inv;
    FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) { inv.serve(fd, req); });

    const std::string db = "/tmp/solarman_period_test_" + std::to_string(getpid()) + ".sqlite3";
    InverterState state;
    {
        History history(db, 30);
        InverterConfig cfg;
        cfg.name = "main";
        cfg.host = "127.0.0.1";
        cfg.port = logger.port();
        cfg.serial = kSerial;
        cfg.poll_period_ms = 20;
        Poller poller(d, cfg, *find_model("deye_sg04lp3"), state, history,
                      history.inverter_id("main"), std::chrono::seconds(10));
        d.add_timer(10, [&] {
            if (state.snapshot().stats.cycles_ok >= 10) d.stop();
        });
        d.run();

        const auto now = std::chrono::system_clock::now();
        EXPECT_EQ(history.range(history.inverter_id("main"), now - std::chrono::minutes(1),
                                now + std::chrono::minutes(1), 0).size(), 1u);
    }
    for (const char* sfx : {"", "-wal", "-shm"}) std::remove((db + sfx).c_str());
    EXPECT_GE(state.snapshot().stats.cycles_ok, 10u);
}
