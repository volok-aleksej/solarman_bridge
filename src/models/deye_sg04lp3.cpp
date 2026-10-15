#include "inverter_model.hpp"

// Deye three-phase low-voltage hybrid, SG04LP3 family (device type 0x0005),
// read as holding registers.

namespace {

// Temperatures are reported with a +100.0 °C offset, in 0.1 °C.
constexpr int kTempOffsetDc = 1000;

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

const char* state_name(uint16_t state) {
    switch (state) {
        case 0:  return "standby";
        case 1:  return "selfcheck";
        case 2:  return "normal";
        case 3:  return "alarm";
        case 4:  return "fault";
        default: return "unknown";
    }
}

bool running(uint16_t state) { return state == 2; }

const InverterModel kModel{
    "deye_sg04lp3",
    "Deye three-phase LV hybrid (SUN-*K-SG04LP3)",
    5,
    {0, 8},   // type, serial
    {
        // Every read costs ~130 ms of logger round trip on top of the
        // data, so the blocks are as large as the Modbus limit allows.
        {62, 3},      // clock
        {500, 60},    // state, daily/total energy counters, temperatures, faults
        {586, 120},   // battery, grid, inverter output, load, PV
    },
    decode_identity,
    decode_clock,
    decode_sample,
    state_name,
    running,
    {
        // Register 80 (0x0050) bit 0 is the remote power switch (ha-solarman
        // deye_p3 "On/Off Switch"). Off stops inverting and charging; the
        // display stays alive.
        {"power_on",  "turn the inverter on",  80, 0x0001, 0x0001},
        {"power_off", "turn the inverter off", 80, 0x0001, 0x0000},
    },
};

const ModelRegistrar kRegistrar{kModel};

}  // namespace
