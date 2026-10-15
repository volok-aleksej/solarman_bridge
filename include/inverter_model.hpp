#pragma once

#include "inverter_commands.hpp"
#include "inverter_data.hpp"

#include <optional>
#include <string>
#include <vector>

// Everything specific to one inverter model. A model is one file in
// src/models/ that defines an InverterModel and registers it:
//
//   static const ModelRegistrar kRegistrar{kModel};
//
// The config picks it by name: logger { model = "..." }.
struct InverterModel {
    const char* name;
    const char* description;
    // Type code decode_identity() reports for this model; anything else
    // only logs a warning. 0 = no check.
    uint16_t device_type;

    RegisterBlock              identity;   // read until known
    std::vector<RegisterBlock> blocks;     // read on every cycle

    InverterIdentity (*decode_identity)(const RegisterBank&);
    // nullopt when the model has no clock or it reads as garbage; the day
    // key then falls back to the host clock.
    std::optional<InverterClock> (*decode_clock)(const RegisterBank&);
    InverterSample (*decode_sample)(const RegisterBank&);

    const char* (*state_name)(uint16_t run_state);
    bool        (*running)(uint16_t run_state);   // the "ON" on the page

    std::vector<InverterCommand> commands;

    const InverterCommand* find_command(const std::string& name) const;
};

// Adds a model to the list the config chooses from. Only for the static
// object in a model's file.
struct ModelRegistrar {
    explicit ModelRegistrar(const InverterModel& model);
};

const std::vector<const InverterModel*>& all_models();
const InverterModel* find_model(const std::string& name);
