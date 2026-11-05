#include "inverter_settings.hpp"

#include <cmath>
#include <cstdlib>

namespace {

// Position of the lowest set bit; the mask is never 0.
int shift_of(uint16_t mask) {
    int n = 0;
    while (!(mask & 1)) { mask >>= 1; ++n; }
    return n;
}

}  // namespace

namespace settings {

uint16_t raw(const SettingDef& def, const RegisterBank& bank) {
    return (bank.u16(def.regs.front()) & def.mask) >> shift_of(def.mask);
}

bool is_on(const SettingDef& def, const RegisterBank& bank) {
    return (bank.u16(def.regs.front()) & def.mask) == def.mask;
}

bool applies(const SettingDef& def, const RegisterBank& bank) {
    return !def.applies || def.applies(bank);
}

std::string check(const std::vector<SettingGroup>& groups, const RegisterBank& want) {
    for (const SettingGroup& g : groups) {
        for (const SettingDef& d : g.defs) {
            if (!d.check || !applies(d, want)) continue;
            if (std::string why = d.check(want); !why.empty()) return why;
        }
    }
    return {};
}

std::pair<int32_t, int32_t> limits(const SettingDef& def, const RegisterBank& bank) {
    return {def.min.raw(bank), def.max.raw(bank)};
}

bool set(const SettingDef& def, const std::string& text, RegisterBank& bank,
         std::string& error) {
    const auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    if (!def.writable) return fail("read-only");

    uint16_t bits = 0;   // of the mask, the same in every register
    if (def.kind == SettingKind::Flag) {
        if (text == "true" || text == "1")       bits = def.mask;
        else if (text != "false" && text != "0") return fail("expected true or false");
    } else {
        char* end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (text.empty() || *end != '\0' || !std::isfinite(number))
            return fail("expected a number");
        const int shift = shift_of(def.mask);
        double raw = number;
        if (def.kind == SettingKind::Choice) {
            if (number != std::floor(number) || number < 0 ||
                number >= static_cast<double>(def.choices.size()))
                return fail("no such choice");
        } else {
            raw = number * def.div;
            if (std::fabs(raw - std::round(raw)) > 1e-6)
                return fail("finer than the setting takes");
            const auto range = limits(def, bank);
            if (raw < range.first || raw > range.second || raw > (def.mask >> shift))
                return fail("out of range");
        }
        bits = static_cast<uint16_t>(std::lround(raw) << shift);
    }
    for (uint16_t reg : def.regs)
        bank.put(reg, {static_cast<uint16_t>((bank.u16(reg) & ~def.mask) | bits)});
    return true;
}

std::vector<RegisterWrite> changes(const RegisterBank& now, const RegisterBank& want,
                                   const std::vector<RegisterBlock>& blocks) {
    std::vector<RegisterWrite> out;
    for (const RegisterBlock& block : blocks) {
        for (uint16_t r = block.addr; r < block.addr + block.count; ++r) {
            if (now.u16(r) == want.u16(r)) continue;
            if (out.empty() || out.back().addr + out.back().values.size() != r)
                out.push_back({r, {}});
            out.back().values.push_back(want.u16(r));
        }
    }
    return out;
}

}  // namespace settings
