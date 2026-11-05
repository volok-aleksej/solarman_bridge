#pragma once

#include "inverter_data.hpp"
#include "inverter_settings.hpp"

#include <string>
#include <vector>

// Everything specific to one inverter model: which registers to read and,
// block by block and setting by setting, what they mean. A model is one
// file in src/models/ that defines an InverterModel and registers it:
//
//   static const ModelRegistrar kRegistrar{kModel};
//
// The config picks it by name: logger { model = "..." }.
struct InverterModel {
    const char* name;
    const char* description;
    uint16_t device_type;

    RegisterBlock              identity;                    // read until known
    std::vector<RegisterBlock> blocks;                      // read on every cycle
    std::vector<RegisterBlock> settings;                    // The settings the Settings page shows, one group per tab. `settings`
                                                            // are the blocks of registers they and their limits use, read one after
                                                            // another into one bank; empty when the model has no settings. Only
                                                            // registers of writable settings are ever written.
    std::vector<SettingGroup>  setting_groups;

    bool has_settings() const { return !settings.empty(); }
    SettingRef find_setting(const std::string& name) const;
};

// Adds a model to the list the config chooses from.
struct ModelRegistrar {
    explicit ModelRegistrar(const InverterModel& model);
};

const std::vector<const InverterModel*>& all_models();
const InverterModel* find_model(const std::string& name);
