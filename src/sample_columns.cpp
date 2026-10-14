#include "sample_columns.hpp"

// Array sizes are fixed by the declarations in the header: adding a column
// without bumping it there is a compile error.
#define COL(name, field, desc)                                         \
    SampleColumn{name, desc,                                           \
                 [](const InverterSample& s) -> int64_t { return s.field; }, \
                 [](InverterSample& s, int64_t v) { s.field = v; }}

const SampleColumn kSampleColumns[] = {
    COL("run_state", run_state,
        "inverter state: 0 standby, 1 selfcheck, 2 normal, 3 alarm, 4 fault (reg 500)"),

    // PV strings
    COL("pv1_power_w", pv_power_w[0],
        "PV string 1 power, W (reg 672)"),
    COL("pv2_power_w", pv_power_w[1],
        "PV string 2 power, W (reg 673)"),
    COL("pv1_voltage_mv", pv_voltage_mv[0],
        "PV string 1 voltage, mV (reg 676, 0.1 V)"),
    COL("pv2_voltage_mv", pv_voltage_mv[1],
        "PV string 2 voltage, mV (reg 678, 0.1 V)"),
    COL("pv1_current_ma", pv_current_ma[0],
        "PV string 1 current, mA (reg 677, 0.1 A)"),
    COL("pv2_current_ma", pv_current_ma[1],
        "PV string 2 current, mA (reg 679, 0.1 A)"),

    // Battery, as the inverter sees it
    COL("batt_voltage_mv", batt_voltage_mv,
        "battery voltage, mV (reg 587, 0.01 V)"),
    COL("batt_current_ma", batt_current_ma,
        "battery current, mA, + discharge / - charge (reg 591, 0.01 A, signed)"),
    COL("batt_power_w", batt_power_w,
        "battery power, W, + discharge / - charge (reg 590, signed)"),
    COL("batt_soc_pct", batt_soc_pct,
        "battery state of charge, % (reg 588)"),
    COL("batt_temp_dc", batt_temp_dc,
        "battery temperature, 0.1 degC (reg 586, offset +100.0 degC removed)"),

    // Grid side
    COL("grid_l1_voltage_mv", grid_voltage_mv[0],
        "grid voltage L1, mV (reg 598, 0.1 V)"),
    COL("grid_l2_voltage_mv", grid_voltage_mv[1],
        "grid voltage L2, mV (reg 599, 0.1 V)"),
    COL("grid_l3_voltage_mv", grid_voltage_mv[2],
        "grid voltage L3, mV (reg 600, 0.1 V)"),
    COL("grid_l1_power_w", grid_power_w[0],
        "grid power L1, W, + import / - export (reg 622, signed)"),
    COL("grid_l2_power_w", grid_power_w[1],
        "grid power L2, W, + import / - export (reg 623, signed)"),
    COL("grid_l3_power_w", grid_power_w[2],
        "grid power L3, W, + import / - export (reg 624, signed)"),
    COL("grid_l1_current_ma", grid_current_ma[0],
        "grid current L1, mA (reg 610, 0.01 A, signed, internal CT)"),
    COL("grid_l2_current_ma", grid_current_ma[1],
        "grid current L2, mA (reg 611, 0.01 A, signed, internal CT)"),
    COL("grid_l3_current_ma", grid_current_ma[2],
        "grid current L3, mA (reg 612, 0.01 A, signed, internal CT)"),
    COL("grid_power_w", grid_power_total_w,
        "grid power total, W, + import / - export (reg 625, signed)"),
    COL("grid_freq_chz", grid_freq_chz,
        "grid frequency, 0.01 Hz (reg 609)"),

    // Inverter AC output
    COL("inv_l1_power_w", inv_power_w[0],
        "inverter output power L1, W (reg 633, signed)"),
    COL("inv_l2_power_w", inv_power_w[1],
        "inverter output power L2, W (reg 634, signed)"),
    COL("inv_l3_power_w", inv_power_w[2],
        "inverter output power L3, W (reg 635, signed)"),
    COL("inv_power_w", inv_power_total_w,
        "inverter output power total, W (reg 636, signed)"),

    // Load (consumers on the inverter's load port)
    COL("load_l1_voltage_mv", load_voltage_mv[0],
        "load voltage L1, mV (reg 644, 0.1 V)"),
    COL("load_l2_voltage_mv", load_voltage_mv[1],
        "load voltage L2, mV (reg 645, 0.1 V)"),
    COL("load_l3_voltage_mv", load_voltage_mv[2],
        "load voltage L3, mV (reg 646, 0.1 V)"),
    COL("load_l1_power_w", load_power_w[0],
        "load power L1, W (reg 650, signed)"),
    COL("load_l2_power_w", load_power_w[1],
        "load power L2, W (reg 651, signed)"),
    COL("load_l3_power_w", load_power_w[2],
        "load power L3, W (reg 652, signed)"),
    COL("load_power_w", load_power_total_w,
        "load power total, W (reg 653, signed)"),

    // Inverter health
    COL("dc_temp_dc", dc_temp_dc,
        "inverter DC side (heatsink) temperature, 0.1 degC (reg 540, offset removed)"),
    COL("ac_temp_dc", ac_temp_dc,
        "inverter AC side temperature, 0.1 degC (reg 541, offset removed)"),
    COL("warning_bits", warning_bits,
        "warning bitmask, 0 = none (regs 553..554, low word first)"),
    COL("fault_bits", fault_bits,
        "fault bitmask, 0 = none (regs 555..558, low word first)"),
};

// The inverter's energy counters change in 0.1 kWh steps and are already
// kept per day in daily_energy, so they are not stored with every sample.
const SampleColumn kCounterColumns[] = {
    // Inverter's own energy counters for the current day (reset at its midnight)
    COL("day_pv_wh", day_pv_wh,
        "PV energy today, Wh (reg 529, 0.1 kWh)"),
    COL("day_load_wh", day_load_wh,
        "load energy today, Wh (reg 526, 0.1 kWh)"),
    COL("day_grid_buy_wh", day_grid_buy_wh,
        "energy bought from grid today, Wh (reg 520, 0.1 kWh)"),
    COL("day_grid_sell_wh", day_grid_sell_wh,
        "energy sold to grid today, Wh (reg 521, 0.1 kWh)"),
    COL("day_batt_charge_wh", day_batt_charge_wh,
        "battery charge energy today, Wh (reg 514, 0.1 kWh)"),
    COL("day_batt_discharge_wh", day_batt_discharge_wh,
        "battery discharge energy today, Wh (reg 515, 0.1 kWh)"),

    // Inverter's lifetime energy counters (32-bit, low word first)
    COL("total_pv_wh", total_pv_wh,
        "PV energy lifetime, Wh (regs 534..535, 0.1 kWh)"),
    COL("total_load_wh", total_load_wh,
        "load energy lifetime, Wh (regs 527..528, 0.1 kWh)"),
    COL("total_grid_buy_wh", total_grid_buy_wh,
        "energy bought from grid lifetime, Wh (regs 522..523, 0.1 kWh)"),
    COL("total_grid_sell_wh", total_grid_sell_wh,
        "energy sold to grid lifetime, Wh (regs 524..525, 0.1 kWh)"),
    COL("total_batt_charge_wh", total_batt_charge_wh,
        "battery charge energy lifetime, Wh (regs 516..517, 0.1 kWh)"),
    COL("total_batt_discharge_wh", total_batt_discharge_wh,
        "battery discharge energy lifetime, Wh (regs 518..519, 0.1 kWh)"),
};

#undef COL
