#pragma once

#include <cstdint>
#include <string>

// A named command the API may run on the inverter. Each model lists its
// own (InverterModel::commands); only those registers are ever written,
// there is no raw register write.
//
// A command owns some bits of one holding register: it reads the register,
// replaces those bits, writes it back with function 0x10 and reads it again
// to verify. The page knows "power_on" and "power_off".
struct InverterCommand {
    const char* name;
    const char* description;
    uint16_t    reg;
    uint16_t    mask;   // bits the command owns
    uint16_t    bits;   // their new value; other bits are kept
};

struct CommandResult {
    bool        ok = false;
    std::string error;
    uint16_t    before = 0;   // read first; decides whether and what to write
    uint16_t    after  = 0;   // read back after the write; verifies it
                              // (= before when no write was needed)
};
