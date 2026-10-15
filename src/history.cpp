#include "history.hpp"

#include "sample_columns.hpp"

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include <cstdio>
#include <iterator>
#include <stdexcept>

namespace {

std::string column_list() {
    std::string s;
    for (const auto& c : kSampleColumns) { s += ", "; s += c.name; }
    return s;
}

// The `-- ...` comments are kept by SQLite in the stored schema text, so
// `.schema` documents the columns right in the database.
std::string schema_sql() {
    std::string s =
        "CREATE TABLE IF NOT EXISTS samples (\n"
        "    id    INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    ts_ms INTEGER NOT NULL,  -- poll time, unix epoch ms (host clock)";
    for (const auto& c : kSampleColumns) {
        s += "\n    ";
        s += c.name;
        s += " INTEGER NOT NULL";
        s += &c == std::end(kSampleColumns) - 1 ? "  -- " : ",  -- ";
        s += c.description;
    }
    s += "\n);\n"
         "CREATE INDEX IF NOT EXISTS idx_samples_ts ON samples(ts_ms);\n"
         "-- One row per inverter calendar day: its daily counters as last seen.\n"
         "CREATE TABLE IF NOT EXISTS daily_energy (\n"
         "    date              TEXT PRIMARY KEY,   -- YYYY-MM-DD, inverter clock\n"
         "    pv_wh             INTEGER NOT NULL,   -- PV energy, Wh\n"
         "    load_wh           INTEGER NOT NULL,   -- load energy, Wh\n"
         "    grid_buy_wh       INTEGER NOT NULL,   -- bought from grid, Wh\n"
         "    grid_sell_wh      INTEGER NOT NULL,   -- sold to grid, Wh\n"
         "    batt_charge_wh    INTEGER NOT NULL,   -- battery charge, Wh\n"
         "    batt_discharge_wh INTEGER NOT NULL,   -- battery discharge, Wh\n"
         "    updated_ms        INTEGER NOT NULL    -- last update, unix epoch ms\n"
         ");\n";
    return s;
}

std::string insert_sql() {
    std::string s = "INSERT INTO samples (ts_ms" + column_list() + ") VALUES (?";
    for (size_t i = 0; i < std::size(kSampleColumns); ++i) s += ",?";
    return s + ")";
}

constexpr const char* kUpsertDailySql = R"sql(
INSERT INTO daily_energy (date, pv_wh, load_wh, grid_buy_wh, grid_sell_wh,
                          batt_charge_wh, batt_discharge_wh, updated_ms)
VALUES (?,?,?,?,?,?,?,?)
ON CONFLICT(date) DO UPDATE SET
    pv_wh             = excluded.pv_wh,
    load_wh           = excluded.load_wh,
    grid_buy_wh       = excluded.grid_buy_wh,
    grid_sell_wh      = excluded.grid_sell_wh,
    batt_charge_wh    = excluded.batt_charge_wh,
    batt_discharge_wh = excluded.batt_discharge_wh,
    updated_ms        = excluded.updated_ms
)sql";

int64_t to_ms(std::chrono::system_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               t.time_since_epoch()).count();
}

}  // namespace

History::History(std::string db_path, int retention_days)
    : db_path_(std::move(db_path)), retention_days_(retention_days) {
    open_db();
}

History::~History() {
    if (insert_stmt_) sqlite3_finalize(insert_stmt_);
    if (daily_stmt_)  sqlite3_finalize(daily_stmt_);
    if (rdb_)         sqlite3_close(rdb_);
    if (db_)          sqlite3_close(db_);
}

void History::open_db() {
    int rc = sqlite3_open_v2(db_path_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        throw std::runtime_error("history: cannot open " + db_path_ + ": " + msg);
    }

    // WAL: the HTTP thread's reads never block the poller's writes.
    auto exec = [&](const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string m = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("history: " + sql + ": " + m);
        }
    };
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous  = NORMAL");
    exec("PRAGMA temp_store   = MEMORY");
    sqlite3_busy_timeout(db_, 5000);
    exec(schema_sql());

    auto prep = [&](const std::string& sql, sqlite3_stmt** out) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, out, nullptr) != SQLITE_OK)
            throw std::runtime_error("history: prepare: " +
                                     std::string(sqlite3_errmsg(db_)));
    };
    prep(insert_sql(),   &insert_stmt_);
    prep(kUpsertDailySql, &daily_stmt_);

    if (sqlite3_open_v2(db_path_.c_str(), &rdb_, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK)
        throw std::runtime_error("history: read-only open failed: " +
                                 std::string(sqlite3_errmsg(rdb_)));
    sqlite3_busy_timeout(rdb_, 2000);

    spdlog::info("history: opened {} (retention {} days)",
                 db_path_, retention_days_);
}

void History::append(const InverterSample& s, const std::string& day) {
    // Old samples are dropped from here, hourly, rather than from a timer.
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_purge_) {
        next_purge_ = now + std::chrono::hours(1);
        purge();
    }

    std::lock_guard<std::mutex> lk(wmtx_);
    const int64_t ts_ms = to_ms(s.ts);

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        spdlog::warn("history: BEGIN failed: {}", sqlite3_errmsg(db_));
        return;
    }

    sqlite3_reset(insert_stmt_);
    int i = 1;
    sqlite3_bind_int64(insert_stmt_, i++, ts_ms);
    for (const auto& c : kSampleColumns)
        sqlite3_bind_int64(insert_stmt_, i++, c.get(s));
    bool ok = sqlite3_step(insert_stmt_) == SQLITE_DONE;

    if (ok) {
        sqlite3_reset(daily_stmt_);
        i = 1;
        sqlite3_bind_text (daily_stmt_, i++, day.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(daily_stmt_, i++, s.day_pv_wh);
        sqlite3_bind_int64(daily_stmt_, i++, s.day_load_wh);
        sqlite3_bind_int64(daily_stmt_, i++, s.day_grid_buy_wh);
        sqlite3_bind_int64(daily_stmt_, i++, s.day_grid_sell_wh);
        sqlite3_bind_int64(daily_stmt_, i++, s.day_batt_charge_wh);
        sqlite3_bind_int64(daily_stmt_, i++, s.day_batt_discharge_wh);
        sqlite3_bind_int64(daily_stmt_, i++, ts_ms);
        ok = sqlite3_step(daily_stmt_) == SQLITE_DONE;
    }

    if (!ok || sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        spdlog::warn("history: append failed: {}", sqlite3_errmsg(db_));
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

void History::purge() {
    std::lock_guard<std::mutex> lk(wmtx_);
    const int64_t cutoff = to_ms(std::chrono::system_clock::now())
                           - int64_t{retention_days_} * 86400000;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM samples WHERE ts_ms < ?", -1,
                           &st, nullptr) != SQLITE_OK) {
        spdlog::warn("history: purge prepare: {}", sqlite3_errmsg(db_));
        return;
    }
    sqlite3_bind_int64(st, 1, cutoff);
    if (sqlite3_step(st) == SQLITE_DONE) {
        const int n = sqlite3_changes(db_);
        if (n > 0) spdlog::info("history: purged {} sample(s)", n);
    } else {
        spdlog::warn("history: purge: {}", sqlite3_errmsg(db_));
    }
    sqlite3_finalize(st);
}

std::vector<InverterSample> History::range(
        std::chrono::system_clock::time_point from,
        std::chrono::system_clock::time_point to, int count) const {
    const int64_t from_ms = to_ms(from);
    const int64_t to_ms_  = to_ms(to);
    const int64_t step_ms = count > 0 ? (to_ms_ - from_ms) / count : 0;

    const std::string cols = "ts_ms" + column_list();
    // Decimation: newest sample per bucket via a window function, a single
    // bounded pass over the ts index.
    const std::string sql = step_ms > 0
        ? "SELECT " + cols + " FROM ("
          "  SELECT *, ROW_NUMBER() OVER ("
          "      PARTITION BY (ts_ms - ?1) / ?3 ORDER BY ts_ms DESC) AS rn"
          "  FROM samples WHERE ts_ms >= ?1 AND ts_ms < ?2"
          ") WHERE rn = 1 ORDER BY ts_ms ASC"
        : "SELECT " + cols + " FROM samples"
          " WHERE ts_ms >= ?1 AND ts_ms < ?2 ORDER BY ts_ms ASC";

    std::lock_guard<std::mutex> lk(rmtx_);
    std::vector<InverterSample> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rdb_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        spdlog::warn("history: range prepare: {}", sqlite3_errmsg(rdb_));
        return out;
    }
    sqlite3_bind_int64(st, 1, from_ms);
    sqlite3_bind_int64(st, 2, to_ms_);
    if (step_ms > 0) sqlite3_bind_int64(st, 3, step_ms);

    while (sqlite3_step(st) == SQLITE_ROW) {
        InverterSample s;
        s.ts = std::chrono::system_clock::time_point(
                   std::chrono::milliseconds(sqlite3_column_int64(st, 0)));
        int i = 1;
        for (const auto& c : kSampleColumns)
            c.set(s, sqlite3_column_int64(st, i++));
        out.push_back(s);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<DailyEnergy> History::daily(int year, int month) const {
    char from[32], to[32];
    snprintf(from, sizeof(from), "%04d-%02d-01", year, month);
    snprintf(to,   sizeof(to),   "%04d-%02d-01", month == 12 ? year + 1 : year,
             month == 12 ? 1 : month + 1);

    std::lock_guard<std::mutex> lk(rmtx_);
    std::vector<DailyEnergy> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rdb_,
            "SELECT date, pv_wh, load_wh, grid_buy_wh, grid_sell_wh,"
            "       batt_charge_wh, batt_discharge_wh"
            " FROM daily_energy WHERE date >= ? AND date < ? ORDER BY date",
            -1, &st, nullptr) != SQLITE_OK) {
        spdlog::warn("history: daily prepare: {}", sqlite3_errmsg(rdb_));
        return out;
    }
    sqlite3_bind_text(st, 1, from, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, to,   -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        DailyEnergy e;
        const unsigned char* d = sqlite3_column_text(st, 0);
        e.date              = d ? reinterpret_cast<const char*>(d) : "";
        e.pv_wh             = sqlite3_column_int64(st, 1);
        e.load_wh           = sqlite3_column_int64(st, 2);
        e.grid_buy_wh       = sqlite3_column_int64(st, 3);
        e.grid_sell_wh      = sqlite3_column_int64(st, 4);
        e.batt_charge_wh    = sqlite3_column_int64(st, 5);
        e.batt_discharge_wh = sqlite3_column_int64(st, 6);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}
