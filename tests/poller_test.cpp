#include "dispatcher.hpp"
#include "history.hpp"
#include "inverter_model.hpp"
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

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
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

std::vector<uint8_t> modbus_regs(const std::vector<uint16_t>& regs, uint8_t slave = 1) {
    std::vector<uint8_t> f = {slave, modbus::kReadHoldingRegisters,
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

TEST(V5Client, TimeoutFailsTheRequestThreeInARowDropTheLink) {
    Harness h;
    FakeLogger logger(h.d, [](int, const std::vector<uint8_t>&) {});  // never answers
    h.start(logger.port(), 100);

    const Result first = h.round_trip(kReadClock);
    EXPECT_FALSE(first.ok);
    EXPECT_EQ(first.error, "receive: timeout");
    EXPECT_TRUE(h.c->connected());   // one silent request is not a dead link

    std::vector<std::string> errors;
    for (int i = 0; i < 2; ++i)
        ASSERT_TRUE(h.c->submit(kReadClock, [&](bool, std::vector<uint8_t>, const std::string& e) {
            errors.push_back(e);
        }, false));
    h.run_until([&] { return errors.size() == 2; });
    EXPECT_EQ(errors[1], "receive: timeout");
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

TEST(V5Client, MissingModbusReplyFailsOnlyThatRequest) {
    Harness h;
    FakeLogger logger(h.d, [&](int fd, const std::vector<uint8_t>& req) {
        // What a logger sends when the inverter on its RS485 stays silent.
        send_bytes(fd, v5_response(v5::kControlResponse, req[5], {}));
    });
    h.start(logger.port());

    const Result r = h.round_trip(kReadClock);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("no Modbus response"), std::string::npos) << r.error;
    EXPECT_TRUE(h.c->connected());
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
        V5Client client(d, "127.0.0.1", logger.port(), kSerial,
                        std::chrono::milliseconds(1000), std::chrono::milliseconds(50), 16);
        InverterConfig cfg;
        cfg.name = "main";
        cfg.poll_period_ms = 50;
        Poller poller(d, client, cfg, *find_model("deye_sg04lp3"), state, history,
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
    EXPECT_EQ(snap.stats.cycles_failed, 0u);
    // Identity once, then clock + 2 live blocks per cycle, on one connection.
    EXPECT_EQ(logger.accepted(), 1);
    EXPECT_GE(requests, 1 + 3 * 3);
}

namespace {

// A fake inverter behind the fake logger: zeroed registers except the
// identity, the clock and register 80, which also takes 0x10 writes.
struct FakeInverter {
    uint16_t reg80 = 1;
    bool     reject_writes = false;
    int      writes = 0;
    int      reads  = 0;

    void serve(int fd, const std::vector<uint8_t>& req) {
        const uint8_t  func = req[27];
        const uint16_t addr = (req[28] << 8) | req[29];
        const uint16_t qty  = (req[30] << 8) | req[31];
        std::vector<uint8_t> mb;
        if (func == modbus::kWriteMultipleRegisters) {
            ++writes;
            if (reject_writes) {
                mb = {1, 0x90, 0x02};
            } else {
                reg80 = (req[33] << 8) | req[34];
                mb = {1, 0x10, req[28], req[29], req[30], req[31]};
            }
            const uint16_t crc = modbus::crc16(mb.data(), mb.size());
            mb.push_back(crc & 0xFF);
            mb.push_back(crc >> 8);
        } else {
            ++reads;
            std::vector<uint16_t> regs(qty, 0);
            if (addr == 0)  regs[0] = 5;
            if (addr == 62) regs = {0x1a0a, 0x0e10, 0x2715};
            if (addr == 80) regs[0] = reg80;
            mb = modbus_regs(regs);
        }
        send_bytes(fd, v5_response(v5::kControlResponse, req[5], mb));
    }
};

class PollerCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = "/tmp/solarman_cmd_test_" + std::to_string(getpid()) + ".sqlite3";
    }
    void TearDown() override {
        for (const char* sfx : {"", "-wal", "-shm"}) std::remove((db_ + sfx).c_str());
    }

    // Runs the poller and submits `cmd` once the first cycle is done.
    CommandResult run(FakeInverter& inv, const char* cmd) {
        Dispatcher d;
        FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) {
            inv.serve(fd, req);
        });
        InverterState state;
        History history(db_, 30);
        V5Client client(d, "127.0.0.1", logger.port(), kSerial,
                        std::chrono::milliseconds(1000), std::chrono::milliseconds(50), 16);
        InverterConfig cfg;
        cfg.name = "main";
        cfg.poll_period_ms = 50;
        Poller poller(d, client, cfg, *find_model("deye_sg04lp3"), state, history,
                      history.inverter_id("main"), std::chrono::milliseconds(0));

        CommandResult result;
        bool submitted = false;
        d.add_timer(10, [&] {
            const auto snap = state.snapshot();
            if (!submitted) {
                if (snap.stats.cycles_ok < 1) return;
                submitted = true;
                EXPECT_TRUE(state.request_command(*find_model("deye_sg04lp3")->find_command(cmd)));
            } else if (snap.last_command) {
                EXPECT_EQ(snap.last_command->name, cmd);
                result = snap.last_command->result;
                d.stop();
            }
        });
        d.run();
        return result;
    }

    std::string db_;
};

}  // namespace

TEST_F(PollerCommandTest, PowerOffWritesAndVerifies) {
    FakeInverter inv;
    const CommandResult r = run(inv, "power_off");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.before, 1);
    EXPECT_EQ(r.after, 0);
    EXPECT_EQ(inv.reg80, 0);
    EXPECT_EQ(inv.writes, 1);
}

TEST_F(PollerCommandTest, NoWriteWhenAlreadyInState) {
    FakeInverter inv;
    const CommandResult r = run(inv, "power_on");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.before, 1);
    EXPECT_EQ(r.after, 1);
    EXPECT_EQ(inv.writes, 0);
}

TEST_F(PollerCommandTest, ReportsModbusException) {
    FakeInverter inv;
    inv.reject_writes = true;
    const CommandResult r = run(inv, "power_off");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("illegal data address"), std::string::npos) << r.error;
    EXPECT_EQ(inv.reg80, 1);
}

TEST_F(PollerCommandTest, KeepsOtherBitsOfTheRegister) {
    FakeInverter inv;
    inv.reg80 = 0x0102;
    const CommandResult r = run(inv, "power_on");
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(inv.reg80, 0x0103);
}


TEST(Poller, StoresOneSamplePerSamplePeriod) {
    Dispatcher d;
    FakeInverter inv;
    FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) { inv.serve(fd, req); });

    const std::string db = "/tmp/solarman_period_test_" + std::to_string(getpid()) + ".sqlite3";
    InverterState state;
    {
        History history(db, 30);
        V5Client client(d, "127.0.0.1", logger.port(), kSerial,
                        std::chrono::milliseconds(1000), std::chrono::milliseconds(50), 16);
        InverterConfig cfg;
        cfg.name = "main";
        cfg.poll_period_ms = 20;
        Poller poller(d, client, cfg, *find_model("deye_sg04lp3"), state, history,
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

TEST(Poller, InvertersOnOneLoggerShareItsConnection) {
    Dispatcher d;
    FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) {
        const uint8_t  slave = req[26];
        const uint16_t addr  = (req[28] << 8) | req[29];
        const uint16_t qty   = (req[30] << 8) | req[31];
        std::vector<uint16_t> regs(qty, 0);
        if (addr == 0)   regs[0] = 5;
        if (addr == 586) regs[588 - 586] = 10 * slave;   // SOC tells them apart
        send_bytes(fd, v5_response(v5::kControlResponse, req[5], modbus_regs(regs, slave)));
    });

    const std::string db = "/tmp/solarman_shared_test_" + std::to_string(getpid()) + ".sqlite3";
    InverterState s1, s2;
    {
        History history(db, 30);
        V5Client client(d, "127.0.0.1", logger.port(), kSerial,
                        std::chrono::milliseconds(1000), std::chrono::milliseconds(50), 32);
        InverterConfig c1, c2;
        c1.name = "one"; c1.slave_id = 1; c1.poll_period_ms = 30;
        c2.name = "two"; c2.slave_id = 2; c2.poll_period_ms = 30;
        const InverterModel& m = *find_model("deye_sg04lp3");
        Poller p1(d, client, c1, m, s1, history, history.inverter_id("one"), std::chrono::milliseconds(0));
        Poller p2(d, client, c2, m, s2, history, history.inverter_id("two"), std::chrono::milliseconds(0));
        d.add_timer(10, [&] {
            if (s1.snapshot().stats.cycles_ok >= 3 && s2.snapshot().stats.cycles_ok >= 3) d.stop();
        });
        d.run();
    }
    for (const char* sfx : {"", "-wal", "-shm"}) std::remove((db + sfx).c_str());

    EXPECT_EQ(s1.snapshot().sample->batt_soc_pct, 10);
    EXPECT_EQ(s2.snapshot().sample->batt_soc_pct, 20);
    EXPECT_EQ(s1.snapshot().stats.cycles_failed, 0u);
    EXPECT_EQ(s2.snapshot().stats.cycles_failed, 0u);
    EXPECT_EQ(logger.accepted(), 1);
}
