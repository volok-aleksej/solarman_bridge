#include "inverter_model.hpp"
#include "modbus_rtu.hpp"
#include "inverter_settings.hpp"
#include "sample_columns.hpp"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const InverterModel& deye() {
    const InverterModel* m = find_model("deye_sg04lp3");
    if (!m) throw std::runtime_error("deye_sg04lp3 is not registered");
    return *m;
}

// What `block` makes of the registers `all` holds: the block is handed
// its own run of them, as after a read, and nothing else.
bool feed(RegisterBlock block, const RegisterBank& all, InverterReadings& out) {
    std::vector<uint16_t> regs;
    for (uint16_t r = block.addr; r < block.addr + block.count; ++r) regs.push_back(all.u16(r));
    return block.on_block_done(regs, out);
}

// One cycle of the model over `all`: the identity block and every cycle block.
InverterReadings read_cycle(const RegisterBank& all) {
    InverterReadings out;
    EXPECT_TRUE(feed(deye().identity, all, out));
    for (const RegisterBlock& b : deye().blocks) EXPECT_TRUE(feed(b, all, out));
    return out;
}

InverterIdentity decode_identity(const RegisterBank& b) { return *read_cycle(b).identity; }
std::optional<InverterClock> decode_clock(const RegisterBank& b) { return read_cycle(b).clock; }
InverterSample decode_sample(const RegisterBank& b) { return read_cycle(b).sample; }

// Zeros for every block the model reads, then patched by the test.
RegisterBank zero_bank() {
    RegisterBank b;
    for (const auto& blk : deye().blocks)
        b.put(blk.addr, std::vector<uint16_t>(blk.count, 0));
    return b;
}

}  // namespace

// ---- every registered model --------------------------------------------------

TEST(Models, RegistryIsSane) {
    ASSERT_FALSE(all_models().empty());
    std::set<std::string> names;
    for (const InverterModel* m : all_models()) {
        EXPECT_TRUE(names.insert(m->name).second) << "duplicate model " << m->name;
        EXPECT_EQ(find_model(m->name), m);
        EXPECT_FALSE(m->blocks.empty()) << m->name;
        for (const RegisterBlock& b : m->blocks) {
            EXPECT_GT(b.count, 0) << m->name;
            EXPECT_LE(b.count, modbus::kMaxReadQuantity) << m->name << " block " << b.addr;
        }
        EXPECT_LE(m->identity.count, modbus::kMaxReadQuantity) << m->name;

        // Settings: every API name is unique and finds its setting, and every
        // register lies in the one block that is read and written.
        EXPECT_EQ(m->has_settings(), !m->setting_groups.empty()) << m->name;
        for (const RegisterBlock& b : m->settings) {
            EXPECT_GT(b.count, 0) << m->name;
            EXPECT_LE(b.count, modbus::kMaxReadQuantity) << m->name << " settings " << b.addr;
        }
        const auto read_with_settings = [&](int reg) {
            for (const RegisterBlock& b : m->settings)
                if (reg >= b.addr && reg < b.addr + b.count) return true;
            return false;
        };
        std::set<std::string> settings, groups;
        for (const SettingGroup& g : m->setting_groups) {
            EXPECT_TRUE(groups.insert(g.id).second) << m->name << " " << g.id;
            for (const SettingDef& d : g.defs) {
                SCOPED_TRACE(std::string(m->name) + " " + d.id);
                EXPECT_TRUE(settings.insert(d.id).second);
                EXPECT_STRNE(d.id, "inverter");   // the API's own parameter
                const SettingRef ref = m->find_setting(d.id);
                ASSERT_TRUE(ref);
                EXPECT_EQ(ref.def, &d);
                EXPECT_EQ(ref.group, &g);
                ASSERT_FALSE(d.regs.empty());
                for (uint16_t reg : d.regs) EXPECT_TRUE(read_with_settings(reg)) << reg;
                EXPECT_NE(d.mask, 0) << d.id;
                if (d.kind == SettingKind::Number) {
                    EXPECT_GT(d.div, 0) << d.id;
                    if (!d.min.of && !d.max.of) { EXPECT_GE(d.max.value, d.min.value) << d.id; }
                }
                if (d.kind == SettingKind::Choice) { EXPECT_FALSE(d.choices.empty()) << d.id; }
            }
        }
        EXPECT_FALSE(m->find_setting("no_such_setting"));
    }
    EXPECT_EQ(find_model("no_such_model"), nullptr);
}

// The identity block and every block of the cycle say what their registers
// mean, and take a read of nothing but zeros.
TEST(Models, EveryBlockOfACycleDecodes) {
    for (const InverterModel* m : all_models()) {
        SCOPED_TRACE(m->name);
        std::vector<RegisterBlock> blocks = m->blocks;
        blocks.push_back(m->identity);
        InverterReadings out;
        for (RegisterBlock& b : blocks) {
            EXPECT_TRUE(b.has_decoder()) << "block " << b.addr;
            EXPECT_TRUE(b.on_block_done(std::vector<uint16_t>(b.count, 0), out)) << "block " << b.addr;
        }
        EXPECT_TRUE(out.identity);
    }
}

// A block keeps what it has read, and sees no register but its own.
TEST(Models, BlockKeepsItsRegisters) {
    RegisterBlock block(10, 3, [](const RegisterBank& regs, InverterReadings& out) {
        out.sample.batt_soc_pct = regs.u16(11) + regs.u16(13);   // 13 is past the block
        return regs.u16(10) != 0;
    });
    InverterReadings out;
    EXPECT_TRUE(block.on_block_done({7, 8, 9}, out));
    EXPECT_EQ(out.sample.batt_soc_pct, 8);
    EXPECT_EQ(block.bank().u16(12), 9);
    EXPECT_FALSE(block.on_block_done({0, 1, 2}, out));    // the decoder's own verdict

    RegisterBlock plain(20, 2);                           // no decoder: only kept
    EXPECT_FALSE(plain.has_decoder());
    EXPECT_TRUE(plain.on_block_done({0x49f0, 2}, out));
    EXPECT_EQ(plain.bank().u32(20), 150000u);
}

// ---- deye_sg04lp3 ------------------------------------------------------------

TEST(DeyeSg04lp3, Identity) {
    RegisterBank b;
    // Real values read from the inverter: type 5, SN "2512233176".
    b.put(0, {5, 1, 260, 12853, 12594, 12851, 13105, 14134});
    const auto id = decode_identity(b);
    EXPECT_EQ(id.device_type, 5);
    EXPECT_EQ(id.serial, "2512233176");
}

TEST(DeyeSg04lp3, Clock) {
    RegisterBank b;
    b.put(62, {0x1a0a, 0x0e10, 0x2715});
    const auto c = decode_clock(b);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->date(), "2026-10-14");
    EXPECT_EQ(c->hour, 16);
    EXPECT_EQ(c->minute, 39);
    EXPECT_EQ(c->second, 21);
}

TEST(DeyeSg04lp3, ClockGarbageRejected) {
    RegisterBank b;
    b.put(62, {0x1a0d, 0x0e10, 0x2715});  // month 13
    EXPECT_FALSE(decode_clock(b));
}

TEST(DeyeSg04lp3, LiveSample) {
    RegisterBank b = zero_bank();
    // Values from a real poll (see the prototype script output).
    b.put(500, {2});
    b.put(514, {3, 9});                // day batt charge / discharge, 0.1 kWh
    b.put(516, {3011, 0, 3151, 0});    // total charge / discharge, u32 low first
    b.put(520, {161, 0});              // day grid buy / sell
    b.put(526, {187});                 // day load
    b.put(529, {35});                  // day PV
    b.put(534, {0x0001, 0x0001});      // total PV = 65537 * 0.1 kWh
    b.put(540, {1250, 1497});          // DC / AC temp
    b.put(586, {1197, 5228, 25, 0, 22, 9});
    b.put(598, {2434, 2382, 2431});
    b.put(609, {4995, 80, 0xFF8E});   // freq, CT current L1, L2 = -1.14 A
    b.put(622, {994, 819, 5, 1818});
    b.put(633, {54, 318, 55, 427});
    b.put(650, {1312, 2074, 94, 3480});
    b.put(672, {197, 299});
    b.put(676, {3090, 6, 3355, 8});

    const auto s = decode_sample(b);
    EXPECT_EQ(s.run_state, 2);
    EXPECT_EQ(s.day_batt_charge_wh, 300);
    EXPECT_EQ(s.day_batt_discharge_wh, 900);
    EXPECT_EQ(s.total_batt_charge_wh, 301100);
    EXPECT_EQ(s.total_pv_wh, 65537LL * 100);
    EXPECT_EQ(s.day_grid_buy_wh, 16100);
    EXPECT_EQ(s.day_load_wh, 18700);
    EXPECT_EQ(s.day_pv_wh, 3500);
    EXPECT_EQ(s.dc_temp_dc, 250);
    EXPECT_EQ(s.ac_temp_dc, 497);
    EXPECT_EQ(s.batt_temp_dc, 197);
    EXPECT_EQ(s.batt_voltage_mv, 52280);
    EXPECT_EQ(s.batt_soc_pct, 25);
    EXPECT_EQ(s.batt_power_w, 22);
    EXPECT_EQ(s.batt_current_ma, 90);
    EXPECT_EQ(s.grid_voltage_mv[1], 238200);
    EXPECT_EQ(s.grid_freq_chz, 4995);
    EXPECT_EQ(s.grid_power_w[0], 994);
    EXPECT_EQ(s.grid_current_ma[0], 800);
    EXPECT_EQ(s.grid_current_ma[1], -1140);
    EXPECT_EQ(s.grid_power_total_w, 1818);
    EXPECT_EQ(s.inv_power_total_w, 427);
    EXPECT_EQ(s.load_power_w[1], 2074);
    EXPECT_EQ(s.load_power_total_w, 3480);
    EXPECT_EQ(s.pv_power_w[1], 299);
    EXPECT_EQ(s.pv_voltage_mv[0], 309000);
    EXPECT_EQ(s.pv_current_ma[1], 800);
}

TEST(DeyeSg04lp3, SignedValues) {
    RegisterBank b = zero_bank();
    b.put(590, {static_cast<uint16_t>(-1500), static_cast<uint16_t>(-2870)});
    b.put(625, {static_cast<uint16_t>(-250)});
    b.put(586, {900});                 // −10.0 °C
    const auto s = decode_sample(b);
    EXPECT_EQ(s.batt_power_w, -1500);
    EXPECT_EQ(s.batt_current_ma, -28700);
    EXPECT_EQ(s.grid_power_total_w, -250);
    EXPECT_EQ(s.batt_temp_dc, -100);
}


// The sample carries the state ready to show: its name and whether it is on.
TEST(DeyeSg04lp3, RunStates) {
    RegisterBank b = zero_bank();
    InverterSample s = decode_sample(b);
    EXPECT_STREQ(s.run_state_name, "standby");
    EXPECT_FALSE(s.running);

    b.put(500, {2});
    s = decode_sample(b);
    EXPECT_EQ(s.run_state, 2);
    EXPECT_STREQ(s.run_state_name, "normal");
    EXPECT_TRUE(s.running);

    b.put(500, {4});
    s = decode_sample(b);
    EXPECT_STREQ(s.run_state_name, "fault");
    EXPECT_FALSE(s.running);

    b.put(500, {77});
    EXPECT_STREQ(decode_sample(b).run_state_name, "unknown");
}

// Each block fills its own part of the readings; the order does not matter.
TEST(DeyeSg04lp3, BlocksDecodeIndependently) {
    RegisterBank b = zero_bank();
    b.put(62, {0x1a0a, 0x0e10, 0x2715});
    b.put(500, {2});
    b.put(588, {77});
    InverterReadings forward, backward;
    for (auto it = deye().blocks.begin(); it != deye().blocks.end(); ++it)
        EXPECT_TRUE(feed(*it, b, forward));
    for (auto it = deye().blocks.rbegin(); it != deye().blocks.rend(); ++it)
        EXPECT_TRUE(feed(*it, b, backward));
    for (const InverterReadings* r : {&forward, &backward}) {
        ASSERT_TRUE(r->clock);
        EXPECT_EQ(r->clock->date(), "2026-10-14");
        EXPECT_TRUE(r->sample.running);
        EXPECT_EQ(r->sample.batt_soc_pct, 77);
        EXPECT_FALSE(r->identity);      // not a block of the cycle
    }
}

// ---- deye_sg04lp3 settings ---------------------------------------------------

namespace {

// The settings registers as read from the inverter on 2026-11-03: the
// rated power (150000 x 0.1 W) and 98..177.
RegisterBank real_settings() {
    RegisterBank b;
    b.put(20, {0x49f0, 0x0002});
    b.put(98, {1, 5760, 5760, 5520, 314, 4500, 20, 90, 0, 5, 90, 150,          // 98..109
               65535, 1, 0, 12, 990, 10, 20, 15, 4600, 5200, 4750, 240,        // 110..121
               0, 4900, 15, 50, 4900, 20, 90, 1, 1, 5500, 0, 0,                // 122..133
               5100, 100, 5400, 0, 1, 500, 3, 1, 1, 14000, 0, 0,               // 134..145
               255, 1,                                                         // 146..147
               100, 500, 900, 1300, 1700, 2100,                                // start
               6000, 6000, 6000, 6000, 6000, 6000,                             // power
               4900, 4900, 4900, 4900, 4900, 4900,                             // voltage
               50, 50, 50, 50, 50, 50,                                         // SOC
               3, 3, 3, 3, 3, 3});                                             // charge
    return b;
}

uint16_t raw_of(const char* name, const RegisterBank& b) {
    const SettingRef ref = deye().find_setting(name);
    if (!ref) throw std::runtime_error(std::string("no setting ") + name);
    return settings::raw(*ref.def, b);
}

}  // namespace

// Read: the rated power and 98..177. Written: nothing below 98, so neither
// the rated power nor register 80, the remote on/off switch, which is not
// even read.
TEST(DeyeSg04lp3, SettingsRegisters) {
    ASSERT_EQ(deye().settings.size(), 2u);
    EXPECT_EQ(deye().settings[0].addr, 20);
    EXPECT_EQ(deye().settings[0].count, 2);
    EXPECT_EQ(deye().settings[1].addr, 98);
    EXPECT_EQ(deye().settings[1].addr + deye().settings[1].count - 1, 177);
    ASSERT_EQ(deye().setting_groups.size(), 3u);
    EXPECT_STREQ(deye().setting_groups[0].id, "tou");
    for (const SettingGroup& g : deye().setting_groups)
        for (const SettingDef& d : g.defs)
            if (d.writable) {
                for (uint16_t reg : d.regs) EXPECT_GE(reg, 98) << d.id;
            }
}

// A time-of-use setting is one value for all six windows: it covers six
// registers in a row, a write sets them all and a read takes the first.
TEST(DeyeSg04lp3, TouSettingsCoverAllSixWindows) {
    const SettingDef& power = *deye().find_setting("tou_power").def;
    EXPECT_EQ(power.regs, (std::vector<uint16_t>{154, 155, 156, 157, 158, 159}));
    EXPECT_EQ(deye().find_setting("tou_soc").def->regs,
              (std::vector<uint16_t>{166, 167, 168, 169, 170, 171}));
    EXPECT_EQ(deye().find_setting("tou_gen_charge").def->regs.size(), 6u);
    EXPECT_FALSE(deye().find_setting("tou1_power"));     // no setting per window

    RegisterBank b = real_settings();
    std::string error;
    ASSERT_TRUE(settings::set(power, "4000", b, error)) << error;
    for (uint16_t reg = 154; reg <= 159; ++reg) EXPECT_EQ(b.u16(reg), 4000);
    EXPECT_EQ(b.u16(160), 4900);                         // the voltages are not touched

    // The generator bit of every window goes off; the grid bit stays.
    ASSERT_TRUE(settings::set(*deye().find_setting("tou_gen_charge").def, "false", b, error))
        << error;
    for (uint16_t reg = 172; reg <= 177; ++reg) EXPECT_EQ(b.u16(reg), 1);

    // Windows that differ: the first one is what is shown.
    b.put(166, {30, 45});
    EXPECT_EQ(raw_of("tou_soc", b), 30);
}

TEST(DeyeSg04lp3, SettingsReadTheRealRegisters) {
    const RegisterBank b = real_settings();
    EXPECT_EQ(raw_of("tou_power", b), 6000);
    EXPECT_EQ(raw_of("tou_soc", b), 50);
    EXPECT_EQ(raw_of("tou_voltage", b), 4900);
    EXPECT_EQ(raw_of("tou_grid_charge", b), 1);
    EXPECT_EQ(raw_of("tou_gen_charge", b), 1);

    EXPECT_EQ(raw_of("grid_charge", b), 1);
    EXPECT_EQ(raw_of("grid_signal", b), 1);
    EXPECT_EQ(raw_of("gen_signal", b), 1);
    EXPECT_EQ(raw_of("grid_start_soc", b), 20);
    EXPECT_EQ(raw_of("grid_current", b), 90);
    EXPECT_EQ(raw_of("gen_start_soc", b), 15);
    EXPECT_EQ(raw_of("gen_current", b), 50);
    EXPECT_EQ(raw_of("gen_run_time", b), 240);

    EXPECT_EQ(raw_of("battery_type", b), 1);
    EXPECT_EQ(raw_of("battery_mode", b), 1);
    EXPECT_EQ(raw_of("capacity", b), 314);
    EXPECT_EQ(raw_of("max_charge_current", b), 90);
    EXPECT_EQ(raw_of("max_discharge_current", b), 150);
    EXPECT_EQ(raw_of("shutdown_soc", b), 10);
    EXPECT_EQ(raw_of("low_soc", b), 15);
    EXPECT_EQ(raw_of("restart_soc", b), 20);
    EXPECT_EQ(raw_of("shutdown_voltage", b), 4600);
    EXPECT_EQ(raw_of("low_voltage", b), 4750);
    EXPECT_EQ(raw_of("restart_voltage", b), 5200);
    EXPECT_EQ(settings::check(deye().setting_groups, b), "");
}

// SOC settings show with a battery managed by SOC, voltage ones otherwise.
TEST(DeyeSg04lp3, SettingsFollowTheBatteryMode) {
    RegisterBank b = real_settings();
    const auto shown = [&](const char* name) {
        return settings::applies(*deye().find_setting(name).def, b);
    };
    EXPECT_TRUE(shown("tou_soc"));
    EXPECT_TRUE(shown("shutdown_soc"));
    EXPECT_TRUE(shown("grid_start_soc"));
    EXPECT_FALSE(shown("tou_voltage"));
    EXPECT_FALSE(shown("shutdown_voltage"));
    EXPECT_TRUE(shown("tou_power"));

    b.put(111, {0});                    // by voltage
    EXPECT_FALSE(shown("tou_soc"));
    EXPECT_FALSE(shown("gen_start_soc"));
    EXPECT_TRUE(shown("tou_voltage"));
    EXPECT_TRUE(shown("restart_voltage"));
    EXPECT_TRUE(shown("max_charge_current"));

    b.put(111, {2});                    // no battery
    EXPECT_FALSE(shown("tou_soc"));
    EXPECT_FALSE(shown("tou_voltage"));
    EXPECT_TRUE(shown("tou_power"));
}

TEST(DeyeSg04lp3, SettingsCheckKeepsTheLevelsInOrder) {
    RegisterBank b = real_settings();
    b.put(115, {16});                   // shutdown above low battery (15)
    EXPECT_NE(settings::check(deye().setting_groups, b), "");
    b.put(115, {15});                   // equal is fine
    EXPECT_EQ(settings::check(deye().setting_groups, b), "");
    b.put(116, {14});                   // restart below low battery
    EXPECT_NE(settings::check(deye().setting_groups, b), "");

    b = real_settings();
    b.put(111, {0});                    // by voltage: SOC levels do not matter
    b.put(115, {90});
    EXPECT_EQ(settings::check(deye().setting_groups, b), "");
    b.put(118, {5300});                 // shutdown above restart (52.00 V)
    EXPECT_NE(settings::check(deye().setting_groups, b), "");
}

// The power of a window goes up to what this very inverter is rated for,
// as registers 20..21 say.
TEST(DeyeSg04lp3, TouPowerLimitComesFromTheRatedPowerRegisters) {
    RegisterBank b = real_settings();
    const SettingDef& power = *deye().find_setting("tou_power").def;
    EXPECT_EQ(settings::limits(power, b).first, 0);
    EXPECT_EQ(settings::limits(power, b).second, 15000);
    b.put(20, {0xd4c0, 0x0001});        // 120000 x 0.1 W
    EXPECT_EQ(settings::limits(power, b).second, 12000);

    const SettingDef& soc = *deye().find_setting("tou_soc").def;   // fixed limits
    EXPECT_EQ(settings::limits(soc, b).second, 100);
}
