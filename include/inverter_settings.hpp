#pragma once

#include "inverter_data.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The inverter's own settings, as the Settings page shows and changes them.
//
// A model describes them once (InverterModel::setting_groups): which
// register and bits hold a value, how it reads, what may be written. The
// API hands that description to the page together with the values and the
// page draws whatever it gets; it knows no setting by name.

enum class SettingKind : uint8_t {
    Number,
    Flag,
    Choice,
};

// A limit of a Number, raw. Either a fixed value, or one the model works
// out from the settings registers as last read.
struct SettingLimit {
    using Of = int32_t (*)(const RegisterBank&);

    SettingLimit(int32_t value = 0) : value(value) {}
    SettingLimit(Of of) : of(of) {}

    int32_t raw(const RegisterBank& bank) const { return of ? of(bank) : value; }

    int32_t value = 0;
    Of      of    = nullptr;
};

struct SettingDef {
    const char* id;                                           // what the API calls the setting; unique within the model
    const char* label;
    SettingKind kind;
    std::vector<uint16_t> regs;                               // The registers of the value. With more than one, the setting is one value
                                                              // for all of them: a write sets every register, a read takes the first.
    uint16_t    mask     = 0xFFFF;                            // the bits of the register it owns
    bool        writable = false;
    bool        (*applies)(const RegisterBank&) = nullptr;    // Checks whether this setting can be used with the other registers as
                                                              // they are set now: a level in volts only while the battery is managed
                                                              // by voltage. nullptr: it always can.
    std::string (*check)(const RegisterBank& want) = nullptr; // Checks whether the new value of this setting agrees with the other
                                                              // registers, before anything is written. `want` holds the registers as
                                                              // they will be after the change. Returns "" when it agrees,
                                                              // otherwise the reason to show.
                                                              // nullptr: nothing to check besides min and max.
    const char*  unit     = "";                               // Number: shown after the value, "W", "%"; no effect on it
    uint16_t     div      = 1;                                // Number: value = raw / div
    SettingLimit min     = 0;                                 // Number: raw limits of a write
    SettingLimit max     = 0;
    std::vector<const char*> choices = {};
};

// Setting group: a tab of the Settings page, a list of settings.
struct SettingGroup {
    const char* id;
    const char* title;
    const char* note;                // shown under the tab; "" for none
    std::vector<SettingDef> defs;
};

// A setting of a model and the group it is in.
struct SettingRef {
    const SettingGroup* group = nullptr;
    const SettingDef*   def   = nullptr;

    explicit operator bool() const { return def != nullptr; }
};

// Registers in a row, written with one 0x10 request.
struct RegisterWrite {
    uint16_t              addr;
    std::vector<uint16_t> values;
};

namespace settings {

// The setting's value as stored: its bits of its first register, shifted down.
uint16_t raw(const SettingDef& def, const RegisterBank& bank);

// Whether a Flag is on, by its first register.
bool is_on(const SettingDef& def, const RegisterBank& bank);

// Checks whether the setting can be used with the other registers as they
// are set now (SettingDef::applies). One that cannot is neither shown nor
// changed.
bool applies(const SettingDef& def, const RegisterBank& bank);

// Runs the check of every setting (SettingDef::check) on `want`, the
// registers as they will be after a change. Returns "" when all of them
// agree, otherwise the reason of the first one that does not. Settings
// that cannot be used now (applies) are skipped.
std::string check(const std::vector<SettingGroup>& groups, const RegisterBank& want);

// The raw limits of a Number as the registers have them now, {min, max}.
std::pair<int32_t, int32_t> limits(const SettingDef& def, const RegisterBank& bank);

// Sets `def` to the value the API gave as text ("true", "0", "12.5"): puts
// it into every register of the setting in `bank`, keeping the bits of
// those registers that are not its own. The limits are taken from `bank`.
// False, with `error`, when the setting is read-only or the text is not a
// value it takes; `bank` is left as it was then.
bool set(const SettingDef& def, const std::string& text, RegisterBank& bank,
         std::string& error);

// The registers of `blocks` that differ between `now` and `want`, as runs of neighbours: one write each.
std::vector<RegisterWrite> changes(const RegisterBank& now, const RegisterBank& want,
                                   const std::vector<RegisterBlock>& blocks);

}  // namespace settings
