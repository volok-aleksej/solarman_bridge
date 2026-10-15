#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Model-independent inverter data. A model (see inverter_model.hpp) reads
// registers into a RegisterBank and decodes them into these structures;
// whatever a model does not have stays 0.
//
// Units follow the bms_bridge convention: integers with the unit in the
// field name (mv, ma, w, wh, dc = 0.1 °C, chz = 0.01 Hz).

struct RegisterBlock {
    uint16_t addr;    // first register
    uint16_t count;   // registers in a row, at most modbus::kMaxReadQuantity
};

constexpr int kPhases = 3;
constexpr int kPvStrings = 2;

struct InverterIdentity {
    uint16_t    device_type = 0;   // model-specific type code
    std::string serial;
};

struct InverterClock {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    std::string date() const;      // "YYYY-MM-DD"
};

struct InverterSample {
    std::chrono::system_clock::time_point ts;

    uint16_t run_state = 0;        // model-specific; see InverterModel::state_name

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

    uint32_t warning_bits = 0;     // model-specific bitmasks, 0 = none
    uint64_t fault_bits   = 0;

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

// The blocks read in one cycle, indexed by register number. A register no
// block covers reads as 0.
class RegisterBank {
public:
    void put(uint16_t addr, const std::vector<uint16_t>& values);
    uint16_t u16(uint16_t addr) const { return addr < regs_.size() ? regs_[addr] : 0; }
    int16_t  s16(uint16_t addr) const { return static_cast<int16_t>(u16(addr)); }
    // Two registers, low word first
    uint32_t u32(uint16_t addr) const { return u16(addr) | (uint32_t{u16(addr + 1)} << 16); }

private:
    std::vector<uint16_t> regs_;
};
