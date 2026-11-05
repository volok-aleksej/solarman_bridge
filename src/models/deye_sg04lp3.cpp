#include "inverter_model.hpp"

// Deye three-phase low-voltage hybrid, SG04LP3 family (device type 0x0005),
// read as holding registers.

namespace {

// Temperatures are reported with a +100.0 °C offset, in 0.1 °C.
constexpr int kTempOffsetDc = 1000;

// ---- blocks ------------------------------------------------------------------
// One decoder per block; each sees only the registers of its own block.

// 0..7: device type and serial number.
bool decode_identity(const RegisterBank& b, InverterReadings& out) {
    InverterIdentity id;
    id.device_type = b.u16(0);
    // Serial number: 10 ASCII characters in regs 3..7, high byte first.
    for (uint16_t r = 3; r <= 7; ++r) {
        const uint16_t v = b.u16(r);
        for (char c : {static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)})
            if (c > ' ' && c < 0x7F) id.serial += c;
    }
    out.identity = std::move(id);
    return true;
}

// 62..64: the inverter's clock. Garbage is not an error: the cycle then
// has no clock and the day comes from the host.
bool decode_clock(const RegisterBank& b, InverterReadings& out) {
    // 62: YY MM, 63: DD hh, 64: mm ss
    InverterClock c;
    c.year   = 2000 + (b.u16(62) >> 8);
    c.month  = b.u16(62) & 0xFF;
    c.day    = b.u16(63) >> 8;
    c.hour   = b.u16(63) & 0xFF;
    c.minute = b.u16(64) >> 8;
    c.second = b.u16(64) & 0xFF;
    if (c.month >= 1 && c.month <= 12 && c.day >= 1 && c.day <= 31 &&
        c.hour <= 23 && c.minute <= 59 && c.second <= 59)
        out.clock = c;
    return true;
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

// 500..559: run state, energy counters, temperatures, warnings and faults.
bool decode_state(const RegisterBank& b, InverterReadings& out) {
    InverterSample& s = out.sample;
    s.run_state      = b.u16(500);
    s.run_state_name = state_name(s.run_state);
    s.running        = s.run_state == 2;

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
    return true;
}

// 586..705: battery, grid, inverter output, load, PV strings.
bool decode_power(const RegisterBank& b, InverterReadings& out) {
    InverterSample& s = out.sample;
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
    return true;
}

// ---- setting conditions, checks and limits -----------------------------------

bool by_soc(const RegisterBank& b)     { return b.u16(111) == 1; }
bool by_voltage(const RegisterBank& b) { return b.u16(111) == 0; }

std::string soc_levels(const RegisterBank& b) {
    if (b.u16(115) <= b.u16(117) && b.u16(117) <= b.u16(116)) return {};
    return "shutdown, low battery and restart SOC must not go down in this order";
}

std::string voltage_levels(const RegisterBank& b) {
    if (b.u16(118) <= b.u16(120) && b.u16(120) <= b.u16(119)) return {};
    return "shutdown, low battery and restart voltage must not go down in this order";
}

int32_t rated_power_w(const RegisterBank& b) { return b.u32(20) / 10; }

// ---- setting makers ----------------------------------------------------------

// Each makes the SettingDef of one sort of setting, so that the list of
// settings below is only a list.

SettingDef number(const char* id, const char* label, uint16_t reg, const char* unit,
                  SettingLimit min, SettingLimit max, uint16_t div = 1) {
    SettingDef d{id, label, SettingKind::Number, {reg}};
    d.writable = true;
    d.unit     = unit;
    d.div      = div;
    d.min      = min;
    d.max      = max;
    return d;
}

// A battery level as voltage, in 0.01 V: only with a battery managed by voltage.
SettingDef volts(const char* id, const char* label, uint16_t reg, int32_t min, int32_t max) {
    SettingDef d = number(id, label, reg, "V", min, max, 100);
    d.applies = by_voltage;
    return d;
}

// A battery level as SOC: only with a battery managed by SOC.
SettingDef percent(const char* id, const char* label, uint16_t reg) {
    SettingDef d = number(id, label, reg, "%", 0, 100);
    d.applies = by_soc;
    return d;
}

// One of the three levels of the ladder, with its check.
SettingDef level(SettingDef d) {
    d.check = d.applies == by_soc ? soc_levels : voltage_levels;
    return d;
}

SettingDef flag(const char* id, const char* label, uint16_t reg, uint16_t mask) {
    SettingDef d{id, label, SettingKind::Flag, {reg}};
    d.mask     = mask;
    d.writable = true;
    return d;
}

SettingDef choice(const char* id, const char* label, uint16_t reg,
                  std::vector<const char*> choices) {
    SettingDef d{id, label, SettingKind::Choice, {reg}};
    d.choices = std::move(choices);
    return d;
}

constexpr int kWindows = 6;

// A time-of-use setting: one value for all six windows. On this inverter
// the windows of a value are six registers in a row; `d` names the first.
SettingDef windows(SettingDef d) {
    const uint16_t first = d.regs[0];
    d.regs.clear();
    for (int i = 0; i < kWindows; ++i) d.regs.push_back(first + i);
    return d;
}

// ---- settings ----------------------------------------------------------------

const std::vector<SettingGroup> kSettingGroups{
    // A window's charge sources are bit 0 (grid) and bit 1 (generator) of
    // one register.
    {"tou", "Time of use",
     "Every value here is set for all six time windows at once; what is shown is "
     "the first window's. Power is the most the battery may give or take. The "
     "battery is discharged down to the level, and charged up to it from the "
     "ticked sources.",
     {
         windows(number("tou_power", "Power", 154, "W", 0, rated_power_w)),
         windows(percent("tou_soc", "SOC", 166)),
         windows(volts("tou_voltage", "Voltage", 160, 3800, 6300)),
         windows(flag("tou_grid_charge", "Grid charge", 172, 0x1)),
         windows(flag("tou_gen_charge", "Generator charge", 172, 0x2)),
     }},
    {"charge", "Charge & signals",
     "Charging starts when the battery is at or below the start level.",
     {
         flag("grid_charge", "Grid charge", 130, 0x1),
         flag("grid_signal", "Grid signal", 140, 0x1),
         percent("grid_start_soc", "Grid charge start", 127),
         volts("grid_start_voltage", "Grid charge start", 126, 3800, 6300),
         number("grid_current", "Grid charge current", 128, "A", 0, 350),
         flag("gen_charge", "Generator charge", 129, 0x1),
         flag("gen_signal", "Generator signal", 140, 0x2),
         percent("gen_start_soc", "Generator charge start", 124),
         volts("gen_start_voltage", "Generator charge start", 123, 3800, 6300),
         number("gen_current", "Generator charge current", 125, "A", 0, 240),
         number("gen_run_time", "Generator max run time", 121, "h", 0, 240, 10),
         number("gen_down_time", "Generator down time", 122, "h", 0, 240, 10),
     }},
    {"battery", "Battery limits", "",
     {
         choice("battery_type", "Battery type", 98, {"Lead-acid", "Lithium"}),
         choice("battery_mode", "Managed by", 111, {"Voltage", "SOC", "No battery"}),
         number("capacity", "Capacity", 102, "Ah", 0, 2000),
         number("max_charge_current", "Max charge current", 108, "A", 0, 350),
         number("max_discharge_current", "Max discharge current", 109, "A", 0, 350),
         level(percent("shutdown_soc", "Shutdown", 115)),
         level(percent("low_soc", "Low battery", 117)),
         level(percent("restart_soc", "Restart", 116)),
         level(volts("shutdown_voltage", "Shutdown", 118, 3800, 6100)),
         level(volts("low_voltage", "Low battery", 120, 3800, 6100)),
         level(volts("restart_voltage", "Restart", 119, 3800, 6100)),
     }},
};

const InverterModel kModel{
    "deye_sg04lp3",
    "Deye three-phase LV hybrid (SUN-*K-SG04LP3)",
    5,
    {0, 8, decode_identity},
    {
        // Every read costs ~130 ms of logger round trip on top of the
        // data, so the blocks are as large as the Modbus limit allows.
        {62, 3, decode_clock},
        {500, 60, decode_state},
        {586, 120, decode_power},
    },
    {
        {20, 2},    // rated power
        {98, 80},   // 98..177: battery and time of use
    },
    kSettingGroups,
};

const ModelRegistrar kRegistrar{kModel};

}  // namespace
