#include "dispatcher.hpp"
#include "history.hpp"
#include "inverter_commands.hpp"
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

// Connect, send one request, run the loop until the reply arrives.
Result round_trip(Dispatcher& d, V5Client& c) {
    Result r;
    c.connect([&](bool ok, const std::string& e) {
        if (!ok) { r = {true, false, {}, e}; d.stop(); return; }
        c.request(kReadClock, [&](bool ok2, std::vector<uint8_t> mb,
                                  const std::string& e2) {
            r = {true, ok2, std::move(mb), e2};
            d.stop();
        });
    });
    d.run();
    return r;
}

}  // namespace

TEST(V5Client, SkipsCounterAndStaleFramesThenReturnsReply) {
    Dispatcher d;
    const auto regs = modbus_regs({0x1a0a, 0x0e10, 0x2715});
    FakeLogger logger(d, [&](int fd, const std::vector<uint8_t>& req) {
        const uint8_t seq = req[5];
        // Modbus frame of the request starts after header + 15 bytes.
        EXPECT_EQ(std::vector<uint8_t>(req.begin() + 26, req.end() - 2), kReadClock);
        send_bytes(fd, v5_response(v5::kControlCounter, seq, {}));
        send_bytes(fd, v5_response(v5::kControlResponse, seq - 1, regs));
        send_bytes(fd, v5_response(v5::kControlResponse, seq, regs));
    });
    V5Client c(d, "127.0.0.1", logger.port(), kSerial, std::chrono::milliseconds(1000));

    const Result r = round_trip(d, c);
    ASSERT_TRUE(r.done);
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.modbus, regs);
    EXPECT_TRUE(c.idle());
}

TEST(V5Client, TimesOutAndCloses) {
    Dispatcher d;
    FakeLogger logger(d, [](int, const std::vector<uint8_t>&) {});  // never answers
    V5Client c(d, "127.0.0.1", logger.port(), kSerial, std::chrono::milliseconds(100));

    const Result r = round_trip(d, c);
    ASSERT_TRUE(r.done);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "receive: timeout");
    EXPECT_FALSE(c.connected());
}

TEST(V5Client, ReportsPeerClose) {
    Dispatcher d;
    FakeLogger* lp = nullptr;
    FakeLogger logger(d, [&](int, const std::vector<uint8_t>&) { lp->drop_client(); });
    lp = &logger;
    V5Client c(d, "127.0.0.1", logger.port(), kSerial, std::chrono::milliseconds(1000));

    const Result r = round_trip(d, c);
    ASSERT_TRUE(r.done);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "receive: logger closed the connection");
    EXPECT_FALSE(c.connected());
}

TEST(V5Client, ConnectRefusedIsReportedAsynchronously) {
    Dispatcher d;
    uint16_t port;
    {
        // A port that was just free: nothing listens there now.
        FakeLogger tmp(d, [](int, const std::vector<uint8_t>&) {});
        port = tmp.port();
    }
    V5Client c(d, "127.0.0.1", port, kSerial, std::chrono::milliseconds(1000));
    bool called = false;
    bool ok = true;
    c.connect([&](bool o, const std::string&) { called = true; ok = o; d.stop(); });
    EXPECT_FALSE(called);  // never from inside connect()
    d.run();
    EXPECT_TRUE(called);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(c.connected());
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
        LoggerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = logger.port();
        cfg.serial = kSerial;
        cfg.poll_period_ms = 50;
        Poller poller(d, cfg, state, history);
        d.add_timer(10, [&] {
            if (state.snapshot().stats.cycles_ok >= 3) d.stop();
        });
        d.run();

        const auto now  = std::chrono::system_clock::now();
        const auto rows = history.range(now - std::chrono::minutes(1),
                                        now + std::chrono::minutes(1), 0);
        EXPECT_GE(rows.size(), 3u);
        const auto days = history.daily(2026, 10);
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
    // Identity once, then clock + 3 live blocks per cycle, on one connection.
    EXPECT_EQ(logger.accepted(), 1);
    EXPECT_GE(requests, 1 + 3 * 4);
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
        LoggerConfig cfg;
        cfg.host = "127.0.0.1";
        cfg.port = logger.port();
        cfg.serial = kSerial;
        cfg.poll_period_ms = 1000;
        Poller poller(d, cfg, state, history);

        CommandResult result;
        bool submitted = false;
        d.add_timer(10, [&] {
            if (submitted || state.snapshot().stats.cycles_ok < 1) return;
            submitted = true;
            EXPECT_TRUE(poller.submit(*find_command(cmd), [&](const CommandResult& r) {
                result = r;
                d.stop();
            }));
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

TEST(InverterCommands, Whitelist) {
    ASSERT_NE(find_command("power_on"), nullptr);
    EXPECT_EQ(find_command("power_on")->reg, 80);
    EXPECT_EQ(find_command("reboot"), nullptr);
}
