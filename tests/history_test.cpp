#include "history.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

using namespace std::chrono;

class HistoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = "/tmp/solarman_history_test_" + std::to_string(getpid()) + ".sqlite3";
        cleanup();
    }
    void TearDown() override { cleanup(); }
    void cleanup() {
        for (const char* sfx : {"", "-wal", "-shm"})
            std::remove((path_ + sfx).c_str());
    }
    std::string path_;
};

InverterSample sample_at(system_clock::time_point ts, int pv) {
    InverterSample s;
    s.ts = ts;
    s.pv_power_w[0]      = pv;
    s.batt_power_w       = -pv;
    s.fault_bits         = 0x8000000000000001ULL;
    s.grid_power_total_w = -3210;
    s.day_pv_wh          = pv * 10;
    s.total_grid_buy_wh  = 25310000;
    return s;
}

}  // namespace

TEST_F(HistoryTest, RoundTripsEveryColumn) {
    History h(path_, 30);
    const int id = h.inverter_id("main");
    const auto t = system_clock::time_point(milliseconds(1'760'000'000'000));
    h.append(id, sample_at(t, 1234), "2025-10-09");

    const auto rows = h.range(id, t - seconds(1), t + seconds(1), 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].ts, t);
    EXPECT_EQ(rows[0].pv_power_w[0], 1234);
    EXPECT_EQ(rows[0].batt_power_w, -1234);
    EXPECT_EQ(rows[0].fault_bits, 0x8000000000000001ULL);
    EXPECT_EQ(rows[0].grid_power_total_w, -3210);
    // Energy counters live in daily_energy, not in samples.
    EXPECT_EQ(rows[0].day_pv_wh, 0);
    EXPECT_EQ(rows[0].total_grid_buy_wh, 0);
}

TEST_F(HistoryTest, DecimatesToNewestPerBucket) {
    History h(path_, 30);
    const int id = h.inverter_id("main");
    const auto t0 = system_clock::time_point(milliseconds(1'760'000'000'000));
    for (int i = 0; i < 100; ++i)
        h.append(id, sample_at(t0 + seconds(i), i), "2025-10-09");

    const auto rows = h.range(id, t0, t0 + seconds(100), 10);
    ASSERT_EQ(rows.size(), 10u);
    EXPECT_EQ(rows.front().pv_power_w[0], 9);    // newest of bucket 0..9
    EXPECT_EQ(rows.back().pv_power_w[0], 99);
    EXPECT_LT(rows.front().ts, rows.back().ts);  // oldest first
}

TEST_F(HistoryTest, DailyKeepsLastCountersOfEachDay) {
    History h(path_, 30);
    const int id = h.inverter_id("main");
    const auto t = system_clock::now();
    h.append(id, sample_at(t, 100), "2026-10-13");
    h.append(id, sample_at(t, 250), "2026-10-13");
    h.append(id, sample_at(t, 7),   "2026-10-14");
    h.append(id, sample_at(t, 1),   "2026-11-01");

    const auto oct = h.daily(id, 2026, 10);
    ASSERT_EQ(oct.size(), 2u);
    EXPECT_EQ(oct[0].date, "2026-10-13");
    EXPECT_EQ(oct[0].pv_wh, 2500);
    EXPECT_EQ(oct[1].date, "2026-10-14");
    EXPECT_EQ(oct[1].pv_wh, 70);
    EXPECT_EQ(h.daily(id, 2026, 11).size(), 1u);
}

TEST_F(HistoryTest, PurgeDropsOldSamplesOnly) {
    History h(path_, 1);
    const int id = h.inverter_id("main");
    const auto now = system_clock::now();
    h.append(id, sample_at(now - hours(48), 1), "2026-10-12");
    h.append(id, sample_at(now, 2), "2026-10-14");
    h.purge();

    const auto rows = h.range(id, now - hours(72), now + seconds(1), 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].pv_power_w[0], 2);
    EXPECT_EQ(h.daily(id, 2026, 10).size(), 2u);     // daily rows are kept
}

TEST_F(HistoryTest, KeepsInvertersApartAndSumsDailyForAll) {
    History h(path_, 30);
    const int a = h.inverter_id("a");
    const int b = h.inverter_id("b");
    EXPECT_NE(a, b);
    EXPECT_EQ(h.inverter_id("a"), a);   // stable, not re-created

    const auto t = system_clock::time_point(milliseconds(1'760'000'000'000));
    h.append(a, sample_at(t, 100), "2026-10-14");
    h.append(b, sample_at(t, 30),  "2026-10-14");

    ASSERT_EQ(h.range(a, t - seconds(1), t + seconds(1), 0).size(), 1u);
    EXPECT_EQ(h.range(b, t - seconds(1), t + seconds(1), 0)[0].pv_power_w[0], 30);
    EXPECT_EQ(h.daily(a, 2026, 10)[0].pv_wh, 1000);
    const auto all = h.daily(0, 2026, 10);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].pv_wh, 1300);
}
