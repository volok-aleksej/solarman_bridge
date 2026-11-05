#include "inverter_settings.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

SettingDef number(uint16_t reg, int32_t min, int32_t max, uint16_t div = 1) {
    SettingDef d{"n", "Number", SettingKind::Number, {reg}};
    d.writable = true;
    d.div = div;
    d.min = min;
    d.max = max;
    return d;
}

SettingDef flag(uint16_t reg, uint16_t mask) {
    SettingDef d{"f", "Flag", SettingKind::Flag, {reg}};
    d.mask = mask;
    d.writable = true;
    return d;
}

// The error of setting `d` to `text` in `bank`, or "".
std::string set(const SettingDef& d, const char* text, RegisterBank& bank) {
    std::string error;
    return settings::set(d, text, bank, error) ? "" : error;
}

// Limits worked out from the registers: a two-register value in 0.1 units,
// and another setting as it is.
int32_t rated_power(const RegisterBank& b) { return b.u32(20) / 10; }
int32_t reg_108(const RegisterBank& b)     { return b.u16(108); }

}  // namespace

TEST(Settings, NumberIsScaledAndCheckedAgainstTheRange) {
    const SettingDef d = number(118, 3800, 6100, 100);   // 0.01 V
    RegisterBank bank;
    EXPECT_EQ(set(d, "46", bank), "");
    EXPECT_EQ(bank.u16(118), 4600);
    EXPECT_EQ(set(d, "47.55", bank), "");
    EXPECT_EQ(bank.u16(118), 4755);
    EXPECT_EQ(set(d, "38", bank), "");
    EXPECT_EQ(set(d, "61", bank), "");
    EXPECT_EQ(bank.u16(118), 6100);

    EXPECT_EQ(set(d, "37.99", bank), "out of range");
    EXPECT_EQ(set(d, "61.01", bank), "out of range");
    EXPECT_EQ(set(d, "-1", bank), "out of range");
    EXPECT_EQ(set(d, "47.555", bank), "finer than the setting takes");
    EXPECT_EQ(set(d, "", bank), "expected a number");
    EXPECT_EQ(set(d, "47V", bank), "expected a number");
    EXPECT_EQ(set(d, "nan", bank), "expected a number");
    EXPECT_EQ(bank.u16(118), 6100);                      // a refused value changes nothing
}

// A setting with several registers is one value for all of them: a write
// sets every register, a read takes the first. They need not be neighbours.
TEST(Settings, OneValueGoesToEveryRegisterOfTheSetting) {
    SettingDef d = number(0, 0, 15000);
    d.regs = {154, 300, 12};
    RegisterBank bank;
    bank.put(12, {111});
    bank.put(154, {222});
    bank.put(300, {333});
    EXPECT_EQ(settings::raw(d, bank), 222);            // the first register

    EXPECT_EQ(set(d, "15001", bank), "out of range");
    EXPECT_EQ(bank.u16(12), 111);
    EXPECT_EQ(set(d, "6000", bank), "");
    EXPECT_EQ(bank.u16(154), 6000);
    EXPECT_EQ(bank.u16(300), 6000);
    EXPECT_EQ(bank.u16(12), 6000);
    EXPECT_EQ(bank.u16(155), 0);                       // nothing else
}

// Either limit of any number may come from the registers instead of being fixed.
TEST(Settings, LimitsMayComeFromTheRegisters) {
    SettingDef d = number(154, 0, 0);
    d.max = rated_power;
    RegisterBank bank;
    bank.put(20, {0x49f0, 0x0002});     // 150000 x 0.1 W
    EXPECT_EQ(settings::limits(d, bank).second, 15000);
    EXPECT_EQ(set(d, "15000", bank), "");
    EXPECT_EQ(set(d, "15001", bank), "out of range");
    bank.put(20, {0xd4c0, 0x0001});     // a 12 kW inverter
    EXPECT_EQ(set(d, "12001", bank), "out of range");
    // Registers not read: the limit is 0 and nothing above it is taken.
    RegisterBank empty;
    EXPECT_EQ(set(d, "100", empty), "out of range");

    // A lower limit that follows another setting.
    SettingDef floor = number(109, 0, 350);
    floor.min = reg_108;
    bank.put(108, {90});
    EXPECT_EQ(settings::limits(floor, bank).first, 90);
    EXPECT_EQ(set(floor, "89", bank), "out of range");
    EXPECT_EQ(set(floor, "90", bank), "");
}

TEST(Settings, FixedLimitsDoNotDependOnTheRegisters) {
    const SettingDef d = number(166, 0, 100);
    const auto range = settings::limits(d, RegisterBank{});
    EXPECT_EQ(range.first, 0);
    EXPECT_EQ(range.second, 100);
}

TEST(Settings, FlagOwnsOnlyItsBits) {
    const SettingDef d = flag(172, 0x2);
    RegisterBank bank;
    bank.put(172, {0x0001});
    EXPECT_FALSE(settings::is_on(d, bank));
    EXPECT_EQ(set(d, "true", bank), "");
    EXPECT_EQ(bank.u16(172), 0x0003);   // bit 0 kept
    EXPECT_TRUE(settings::is_on(d, bank));
    EXPECT_EQ(settings::raw(d, bank), 1);
    EXPECT_EQ(set(d, "0", bank), "");
    EXPECT_EQ(bank.u16(172), 0x0001);
    EXPECT_EQ(set(d, "yes", bank), "expected true or false");
    EXPECT_EQ(set(d, "", bank), "expected true or false");
}

TEST(Settings, ReadOnlySettingsTakeNothing) {
    SettingDef d = number(102, 0, 2000);
    d.writable = false;
    RegisterBank bank;
    EXPECT_EQ(set(d, "100", bank), "read-only");
    EXPECT_EQ(bank.u16(102), 0);
}

TEST(Settings, ChoiceTakesAnIndex) {
    SettingDef d{"mode", "Mode", SettingKind::Choice, {111}};
    d.writable = true;
    d.choices  = {"Voltage", "SOC", "No battery"};
    RegisterBank bank;
    EXPECT_EQ(set(d, "2", bank), "");
    EXPECT_EQ(bank.u16(111), 2);
    EXPECT_EQ(set(d, "3", bank), "no such choice");
    EXPECT_EQ(set(d, "1.5", bank), "no such choice");
    EXPECT_EQ(set(d, "SOC", bank), "expected a number");
}

// A setting says itself whether the inverter has it and whether it fits.
TEST(Settings, SettingsApplyAndCheckByTheirOwnFunctions) {
    SettingDef plain = number(108, 0, 350);
    SettingDef soc = number(115, 0, 100);
    soc.applies = [](const RegisterBank& b) { return b.u16(111) == 1; };
    soc.check   = [](const RegisterBank& b) {
        return b.u16(115) <= b.u16(117) ? std::string() : std::string("above low battery");
    };
    const std::vector<SettingGroup> groups{
        {"battery", "Battery", "", {plain, soc}},
    };

    RegisterBank b;
    b.put(111, {1});
    b.put(115, {10});
    b.put(117, {15});
    EXPECT_TRUE(settings::applies(plain, b));
    EXPECT_TRUE(settings::applies(soc, b));
    EXPECT_EQ(settings::check(groups, b), "");

    b.put(115, {20});
    EXPECT_EQ(settings::check(groups, b), "above low battery");
    b.put(111, {0});                                // not managed by SOC: no such setting
    EXPECT_FALSE(settings::applies(soc, b));
    EXPECT_EQ(settings::check(groups, b), "");
}

TEST(Settings, ChangesAreRunsOfNeighbours) {
    RegisterBank now;
    now.put(98, std::vector<uint16_t>(80, 7));
    RegisterBank want = now;
    EXPECT_TRUE(settings::changes(now, want, {{20, 2}, {98, 80}}).empty());

    // Power of all six windows, SOC of two of them, one lone register.
    want.put(154, {1, 2, 3, 4, 5, 6});
    want.put(166, {40});
    want.put(168, {60});
    want.put(108, {7});                                 // already 7: no write
    const auto w = settings::changes(now, want, {{20, 2}, {98, 80}});
    ASSERT_EQ(w.size(), 3u);
    EXPECT_EQ(w[0].addr, 154);
    EXPECT_EQ(w[0].values, (std::vector<uint16_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(w[1].addr, 166);
    EXPECT_EQ(w[1].values, (std::vector<uint16_t>{40}));
    EXPECT_EQ(w[2].addr, 168);
    EXPECT_EQ(w[2].values, (std::vector<uint16_t>{60}));
}
