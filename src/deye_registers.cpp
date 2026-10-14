#include "deye_registers.hpp"

#include <cstdio>
#include <stdexcept>

namespace {

// Temperatures are reported with a +100.0 °C offset, in 0.1 °C.
constexpr int kTempOffsetDc = 1000;

}  // namespace

std::string InverterClock::date() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    return buf;
}

void RegisterBank::put(uint16_t addr, const std::vector<uint16_t>& values) {
    for (size_t i = 0; i < values.size(); ++i)
        regs_[addr + i] = values[i];
}

uint16_t RegisterBank::u16(uint16_t addr) const {
    auto it = regs_.find(addr);
    if (it == regs_.end())
        throw std::out_of_range("register " + std::to_string(addr) + " not read");
    return it->second;
}

int16_t RegisterBank::s16(uint16_t addr) const {
    return static_cast<int16_t>(u16(addr));
}

uint32_t RegisterBank::u32(uint16_t addr) const {
    return u16(addr) | (uint32_t{u16(addr + 1)} << 16);
}

InverterIdentity decode_identity(const RegisterBank& bank) {
    InverterIdentity id;
    id.device_type = bank.u16(0);
    // Serial number: 10 ASCII characters in regs 3..7, high byte first.
    for (uint16_t r = 3; r <= 7; ++r) {
        const uint16_t v = bank.u16(r);
        for (char c : {static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)})
            if (c > ' ' && c < 0x7F) id.serial += c;
    }
    return id;
}

std::optional<InverterClock> decode_clock(const RegisterBank& bank) {
    // 62: YY MM, 63: DD hh, 64: mm ss
    InverterClock c;
    c.year   = 2000 + (bank.u16(62) >> 8);
    c.month  = bank.u16(62) & 0xFF;
    c.day    = bank.u16(63) >> 8;
    c.hour   = bank.u16(63) & 0xFF;
    c.minute = bank.u16(64) >> 8;
    c.second = bank.u16(64) & 0xFF;
    if (c.month < 1 || c.month > 12 || c.day < 1 || c.day > 31 ||
        c.hour > 23 || c.minute > 59 || c.second > 59)
        return std::nullopt;
    return c;
}

InverterSample decode_sample(const RegisterBank& b) {
    InverterSample s;
    s.ts = std::chrono::system_clock::now();

    s.run_state = b.u16(500);

    // Counters in 0.1 kWh
    s.day_batt_charge_wh    = b.u16(514) * 100;
    s.day_batt_discharge_wh = b.u16(515) * 100;
    s.day_grid_buy_wh       = b.u16(520) * 100;
    s.day_grid_sell_wh      = b.u16(521) * 100;
    s.day_load_wh           = b.u16(526) * 100;
    s.day_pv_wh             = b.u16(529) * 100;

    s.total_batt_charge_wh    = b.u32(516) * int64_t{100};
    s.total_batt_discharge_wh = b.u32(518) * int64_t{100};
    s.total_grid_buy_wh       = b.u32(522) * int64_t{100};
    s.total_grid_sell_wh      = b.u32(524) * int64_t{100};
    s.total_load_wh           = b.u32(527) * int64_t{100};
    s.total_pv_wh             = b.u32(534) * int64_t{100};

    s.dc_temp_dc = b.u16(540) - kTempOffsetDc;
    s.ac_temp_dc = b.u16(541) - kTempOffsetDc;

    s.warning_bits = b.u32(553);
    s.fault_bits   = b.u32(555) | (uint64_t{b.u32(557)} << 32);

    s.batt_temp_dc    = b.u16(586) - kTempOffsetDc;
    s.batt_voltage_mv = b.u16(587) * 10;
    s.batt_soc_pct    = b.u16(588);
    s.batt_power_w    = b.s16(590);
    s.batt_current_ma = b.s16(591) * 10;

    for (int p = 0; p < kPhases; ++p) {
        s.grid_voltage_mv[p] = b.u16(598 + p) * 100;
        s.grid_power_w[p]    = b.s16(622 + p);
        s.grid_current_ma[p] = b.s16(610 + p) * 10;
        s.inv_power_w[p]     = b.s16(633 + p);
        s.load_voltage_mv[p] = b.u16(644 + p) * 100;
        s.load_power_w[p]    = b.s16(650 + p);
    }
    s.grid_freq_chz      = b.u16(609);
    s.grid_power_total_w = b.s16(625);
    s.inv_power_total_w  = b.s16(636);
    s.load_power_total_w = b.s16(653);

    for (int i = 0; i < kPvStrings; ++i) {
        s.pv_power_w[i]    = b.u16(672 + i);
        s.pv_voltage_mv[i] = b.u16(676 + 2 * i) * 100;
        s.pv_current_ma[i] = b.u16(677 + 2 * i) * 100;
    }
    return s;
}

const char* run_state_name(uint16_t state) {
    switch (state) {
        case 0:  return "standby";
        case 1:  return "selfcheck";
        case 2:  return "normal";
        case 3:  return "alarm";
        case 4:  return "fault";
        default: return "unknown";
    }
}
