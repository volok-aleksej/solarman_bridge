#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Register map of the Deye three-phase low-voltage hybrid (SG04LP3 family,
// device type 0x0005), read as holding registers through the logger.
//
// Units follow the bms_bridge convention: integers with the unit in the
// field name (mv, ma, w, wh, dc = 0.1 °C, chz = 0.01 Hz).

struct RegisterBlock {
    uint16_t addr;
    uint16_t count;
};

// Device identity, read once after (re)connecting.
inline constexpr RegisterBlock kIdentityBlock{0, 8};

// Read on every poll cycle.
inline constexpr RegisterBlock kClockBlock{62, 3};
inline constexpr RegisterBlock kLiveBlocks[] = {
    {500, 60},   // state, daily/total energy counters, temperatures, faults
    {586, 60},   // battery, grid, inverter output
    {646, 60},   // load, PV
};

constexpr int kPhases = 3;
constexpr int kPvStrings = 2;

struct InverterIdentity {
    uint16_t    device_type = 0;   // 5 = three-phase LV hybrid
    std::string serial;
};

struct InverterClock {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    std::string date() const;      // "YYYY-MM-DD"
};

struct InverterSample {
    std::chrono::system_clock::time_point ts;

    uint16_t run_state = 0;        // 0 standby, 1 selfcheck, 2 normal, 3 alarm, 4 fault

    // PV strings
    int32_t  pv_power_w[kPvStrings]   = {};
    int32_t  pv_voltage_mv[kPvStrings] = {};
    int32_t  pv_current_ma[kPvStrings] = {};

    // Battery (+ discharge / − charge, the inverter's sign convention)
    int32_t  batt_voltage_mv = 0;
    int32_t  batt_current_ma = 0;
    int32_t  batt_power_w    = 0;
    int32_t  batt_soc_pct    = 0;
    int32_t  batt_temp_dc    = 0;

    // Grid (+ import / − export)
    int32_t  grid_voltage_mv[kPhases] = {};
    int32_t  grid_power_w[kPhases]    = {};
    int32_t  grid_current_ma[kPhases] = {};   // inverter's internal CT
    int32_t  grid_power_total_w       = 0;
    int32_t  grid_freq_chz            = 0;

    // Inverter AC output
    int32_t  inv_power_w[kPhases] = {};
    int32_t  inv_power_total_w    = 0;

    // Load
    int32_t  load_voltage_mv[kPhases] = {};
    int32_t  load_power_w[kPhases]    = {};
    int32_t  load_power_total_w       = 0;

    int32_t  dc_temp_dc = 0;
    int32_t  ac_temp_dc = 0;

    uint32_t warning_bits = 0;     // regs 553..554
    uint64_t fault_bits   = 0;     // regs 555..558

    // Energy counters kept by the inverter itself (reset at its midnight)
    int64_t  day_pv_wh             = 0;
    int64_t  day_load_wh           = 0;
    int64_t  day_grid_buy_wh       = 0;
    int64_t  day_grid_sell_wh      = 0;
    int64_t  day_batt_charge_wh    = 0;
    int64_t  day_batt_discharge_wh = 0;

    int64_t  total_pv_wh             = 0;
    int64_t  total_load_wh           = 0;
    int64_t  total_grid_buy_wh       = 0;
    int64_t  total_grid_sell_wh      = 0;
    int64_t  total_batt_charge_wh    = 0;
    int64_t  total_batt_discharge_wh = 0;
};

// Sparse address → value view over the blocks read in one cycle.
class RegisterBank {
public:
    void put(uint16_t addr, const std::vector<uint16_t>& values);
    bool has(uint16_t addr) const { return regs_.count(addr) != 0; }
    uint16_t u16(uint16_t addr) const;
    int16_t  s16(uint16_t addr) const;
    // Two registers, low word first (Deye convention for 32-bit counters)
    uint32_t u32(uint16_t addr) const;

private:
    std::unordered_map<uint16_t, uint16_t> regs_;
};

InverterIdentity decode_identity(const RegisterBank& bank);
std::optional<InverterClock> decode_clock(const RegisterBank& bank);
InverterSample decode_sample(const RegisterBank& bank);

const char* run_state_name(uint16_t state);
