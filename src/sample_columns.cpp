#include "sample_columns.hpp"

// Array sizes are fixed by the declarations in the header: adding a column
// without bumping it there is a compile error.
#define COL(name, field, desc)                                         \
    SampleColumn{name, desc,                                           \
                 [](const InverterSample& s) -> int64_t { return s.field; }, \
                 [](InverterSample& s, int64_t v) { s.field = v; }}

const SampleColumn kSampleColumns[] = {
    COL("run_state", run_state,
        "inverter run state, model-specific code"),

    // PV strings
    COL("pv1_power_w", pv_power_w[0],
        "PV string 1 power, W"),
    COL("pv2_power_w", pv_power_w[1],
        "PV string 2 power, W"),
    COL("pv1_voltage_mv", pv_voltage_mv[0],
        "PV string 1 voltage, mV"),
    COL("pv2_voltage_mv", pv_voltage_mv[1],
        "PV string 2 voltage, mV"),
    COL("pv1_current_ma", pv_current_ma[0],
        "PV string 1 current, mA"),
    COL("pv2_current_ma", pv_current_ma[1],
        "PV string 2 current, mA"),

    // Battery, as the inverter sees it
    COL("batt_voltage_mv", batt_voltage_mv,
        "battery voltage, mV"),
    COL("batt_current_ma", batt_current_ma,
        "battery current, mA, + discharge / - charge"),
    COL("batt_power_w", batt_power_w,
        "battery power, W, + discharge / - charge"),
    COL("batt_soc_pct", batt_soc_pct,
        "battery state of charge, %"),
    COL("batt_temp_dc", batt_temp_dc,
        "battery temperature, 0.1 degC"),

    // Grid side
    COL("grid_l1_voltage_mv", grid_voltage_mv[0],
        "grid voltage L1, mV"),
    COL("grid_l2_voltage_mv", grid_voltage_mv[1],
        "grid voltage L2, mV"),
    COL("grid_l3_voltage_mv", grid_voltage_mv[2],
        "grid voltage L3, mV"),
    COL("grid_l1_power_w", grid_power_w[0],
        "grid power L1, W, + import / - export"),
    COL("grid_l2_power_w", grid_power_w[1],
        "grid power L2, W, + import / - export"),
    COL("grid_l3_power_w", grid_power_w[2],
        "grid power L3, W, + import / - export"),
    COL("grid_l1_current_ma", grid_current_ma[0],
        "grid current L1, mA"),
    COL("grid_l2_current_ma", grid_current_ma[1],
        "grid current L2, mA"),
    COL("grid_l3_current_ma", grid_current_ma[2],
        "grid current L3, mA"),
    COL("grid_power_w", grid_power_total_w,
        "grid power total, W, + import / - export"),
    COL("grid_freq_chz", grid_freq_chz,
        "grid frequency, 0.01 Hz"),

    // Inverter AC output
    COL("inv_l1_power_w", inv_power_w[0],
        "inverter output power L1, W"),
    COL("inv_l2_power_w", inv_power_w[1],
        "inverter output power L2, W"),
    COL("inv_l3_power_w", inv_power_w[2],
        "inverter output power L3, W"),
    COL("inv_power_w", inv_power_total_w,
        "inverter output power total, W"),

    // Load (consumers on the inverter's load port)
    COL("load_l1_voltage_mv", load_voltage_mv[0],
        "load voltage L1, mV"),
    COL("load_l2_voltage_mv", load_voltage_mv[1],
        "load voltage L2, mV"),
    COL("load_l3_voltage_mv", load_voltage_mv[2],
        "load voltage L3, mV"),
    COL("load_l1_power_w", load_power_w[0],
        "load power L1, W"),
    COL("load_l2_power_w", load_power_w[1],
        "load power L2, W"),
    COL("load_l3_power_w", load_power_w[2],
        "load power L3, W"),
    COL("load_power_w", load_power_total_w,
        "load power total, W"),

    // Inverter health
    COL("dc_temp_dc", dc_temp_dc,
        "inverter DC side (heatsink) temperature, 0.1 degC"),
    COL("ac_temp_dc", ac_temp_dc,
        "inverter AC side temperature, 0.1 degC"),
    COL("warning_bits", warning_bits,
        "warning bitmask, 0 = none"),
    COL("fault_bits", fault_bits,
        "fault bitmask, 0 = none"),
};

// The inverter's energy counters change slowly and are already kept per
// day in daily_energy, so they are not stored with every sample.
const SampleColumn kCounterColumns[] = {
    // Inverter's own energy counters for the current day (reset at its midnight)
    COL("day_pv_wh", day_pv_wh,
        "PV energy today, Wh"),
    COL("day_load_wh", day_load_wh,
        "load energy today, Wh"),
    COL("day_grid_buy_wh", day_grid_buy_wh,
        "energy bought from grid today, Wh"),
    COL("day_grid_sell_wh", day_grid_sell_wh,
        "energy sold to grid today, Wh"),
    COL("day_batt_charge_wh", day_batt_charge_wh,
        "battery charge energy today, Wh"),
    COL("day_batt_discharge_wh", day_batt_discharge_wh,
        "battery discharge energy today, Wh"),

    // Inverter's lifetime energy counters
    COL("total_pv_wh", total_pv_wh,
        "PV energy lifetime, Wh"),
    COL("total_load_wh", total_load_wh,
        "load energy lifetime, Wh"),
    COL("total_grid_buy_wh", total_grid_buy_wh,
        "energy bought from grid lifetime, Wh"),
    COL("total_grid_sell_wh", total_grid_sell_wh,
        "energy sold to grid lifetime, Wh"),
    COL("total_batt_charge_wh", total_batt_charge_wh,
        "battery charge energy lifetime, Wh"),
    COL("total_batt_discharge_wh", total_batt_discharge_wh,
        "battery discharge energy lifetime, Wh"),
};

#undef COL
