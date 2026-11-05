#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Model-independent inverter data. A model (see inverter_model.hpp) lists
// blocks of registers; each block decodes what it has read into these
// structures. Whatever a model does not have stays 0.
//
// Units follow the bms_bridge convention: integers with the unit in the
// field name (mv, ma, w, wh, dc = 0.1 °C, chz = 0.01 Hz).

// Holding registers by number. A register that was not put reads as 0.
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

    // The model's own code, kept in the history, and what it means, ready
    // to show. The name is a string literal of the model.
    uint16_t    run_state      = 0;
    const char* run_state_name = "";
    bool        running        = false;   // the "ON" on the page

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

// What the blocks of one poll cycle fill in. The poller makes it when a
// cycle starts and drops it when the cycle is finished.
struct InverterReadings {
    std::optional<InverterIdentity> identity;   // by the identity block
    std::optional<InverterClock>    clock;      // when a block holds a sane one
    InverterSample                  sample;
};

// A run of holding registers read with one request, together with the
// registers as last read and what they mean: its decoder puts them into
// the readings of the cycle. A model lists its blocks; every poller works
// on copies of its own, so what a block holds belongs to one inverter.
class RegisterBlock {
public:
    // False when the registers make no sense; the cycle fails then.
    using Decode = bool (*)(const RegisterBank& regs, InverterReadings& out);

    RegisterBlock(uint16_t addr, uint16_t count, Decode decode = nullptr)
        : addr(addr), count(count), decode_(decode) {}

    uint16_t addr;    // first register
    uint16_t count;   // registers in a row, at most modbus::kMaxReadQuantity

    // Takes the answer to its read and decodes it. A block without a
    // decoder only keeps the registers.
    bool on_block_done(const std::vector<uint16_t>& regs, InverterReadings& out);

    const RegisterBank& bank() const { return bank_; }
    bool has_decoder() const { return decode_ != nullptr; }

private:
    RegisterBank bank_;
    Decode       decode_;
};
