#include "deye_registers.hpp"
#include "sample_columns.hpp"

#include <gtest/gtest.h>

#include <iterator>
#include <vector>

namespace {

// Register bank filled with zeros for every live block, then patched.
RegisterBank zero_bank() {
    RegisterBank b;
    b.put(kClockBlock.addr, std::vector<uint16_t>(kClockBlock.count, 0));
    for (const auto& blk : kLiveBlocks)
        b.put(blk.addr, std::vector<uint16_t>(blk.count, 0));
    return b;
}

}  // namespace

TEST(DeyeRegisters, Identity) {
    RegisterBank b;
    // Real values read from the inverter: type 5, SN "2512233176".
    b.put(0, {5, 1, 260, 12853, 12594, 12851, 13105, 14134});
    const auto id = decode_identity(b);
    EXPECT_EQ(id.device_type, 5);
    EXPECT_EQ(id.serial, "2512233176");
}

TEST(DeyeRegisters, Clock) {
    RegisterBank b;
    b.put(62, {0x1a0a, 0x0e10, 0x2715});
    const auto c = decode_clock(b);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->date(), "2026-10-14");
    EXPECT_EQ(c->hour, 16);
    EXPECT_EQ(c->minute, 39);
    EXPECT_EQ(c->second, 21);
}

TEST(DeyeRegisters, ClockGarbageRejected) {
    RegisterBank b;
    b.put(62, {0x1a0d, 0x0e10, 0x2715});  // month 13
    EXPECT_FALSE(decode_clock(b));
}

TEST(DeyeRegisters, LiveSample) {
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

TEST(DeyeRegisters, SignedValues) {
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

// Every register a decoder uses must lie in a block that is read: with
// the same values, a bank filled only through the blocks must decode
// exactly like a bank with every register set.
TEST(DeyeRegisters, DecodersUseOnlyRegistersThatAreRead) {
    std::vector<uint16_t> all(kRegisterSpan);
    for (size_t i = 0; i < all.size(); ++i) all[i] = 0x1000 + i * 7;

    RegisterBank full, blocks;
    full.put(0, all);
    std::vector<RegisterBlock> read = {kIdentityBlock, kClockBlock};
    read.insert(read.end(), std::begin(kLiveBlocks), std::end(kLiveBlocks));
    for (const RegisterBlock& b : read)
        blocks.put(b.addr, {all.begin() + b.addr, all.begin() + b.addr + b.count});

    const InverterSample a = decode_sample(full), b = decode_sample(blocks);
    for (const auto& c : kSampleColumns)  EXPECT_EQ(c.get(a), c.get(b)) << c.name;
    for (const auto& c : kCounterColumns) EXPECT_EQ(c.get(a), c.get(b)) << c.name;
    EXPECT_EQ(decode_identity(full).serial, decode_identity(blocks).serial);
    EXPECT_EQ(decode_identity(full).device_type, decode_identity(blocks).device_type);
    EXPECT_EQ(decode_clock(full).has_value(), decode_clock(blocks).has_value());
}
