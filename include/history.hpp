#pragma once

#include "inverter_data.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

// One row per inverter calendar day: the inverter's own daily counters as
// last seen that day (they reset at the inverter's midnight).
struct DailyEnergy {
    std::string date;              // "YYYY-MM-DD", inverter clock
    int64_t pv_wh             = 0;
    int64_t load_wh           = 0;
    int64_t grid_buy_wh       = 0;
    int64_t grid_sell_wh      = 0;
    int64_t batt_charge_wh    = 0;
    int64_t batt_discharge_wh = 0;
};

class History {
public:
    History(std::string db_path, int retention_days);
    ~History();

    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // Id of the inverter called `name`, registered on first use. Writer
    // side; call it before the pollers start.
    int inverter_id(const std::string& name);

    // Store a sample and refresh the daily_energy row for `day` from its
    // daily counters; purges old samples once an hour. Called by the
    // pollers (dispatcher thread).
    void append(int inverter_id, const InverterSample& s, const std::string& day);

    // Drop raw samples older than the retention window.
    void purge();

    // Samples in [from, to), oldest first. With count > 0 the window is cut
    // into `count` equal buckets and the newest sample of each is returned.
    std::vector<InverterSample> range(int inverter_id,
                                      std::chrono::system_clock::time_point from,
                                      std::chrono::system_clock::time_point to,
                                      int count) const;

    // Daily energy of one inverter, or with inverter_id 0 of all of them
    // summed per day.
    std::vector<DailyEnergy> daily(int inverter_id, int year, int month) const;

private:
    void open_db();

    std::string db_path_;
    int         retention_days_;
    std::chrono::steady_clock::time_point next_purge_{};

    sqlite3*      db_          = nullptr;   // writer (dispatcher thread)
    sqlite3*      rdb_         = nullptr;   // read-only (HTTP thread)
    sqlite3_stmt* insert_stmt_ = nullptr;
    sqlite3_stmt* daily_stmt_  = nullptr;

    std::mutex         wmtx_;
    mutable std::mutex rmtx_;
};
