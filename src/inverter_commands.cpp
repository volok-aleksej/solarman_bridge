#include "inverter_commands.hpp"

// Register 80 (0x0050) bit 0 is the remote power switch of the Deye
// SG04LP3 (ha-solarman deye_p3 "On/Off Switch"). Off stops inverting and
// charging; the display stays alive. Register 551 reports the result.
const InverterCommand kInverterCommands[] = {
    {"power_on",  "turn the inverter on",  80, 0x0001, 0x0001},
    {"power_off", "turn the inverter off", 80, 0x0001, 0x0000},
};

const InverterCommand* find_command(const std::string& name) {
    for (const auto& c : kInverterCommands)
        if (name == c.name) return &c;
    return nullptr;
}
