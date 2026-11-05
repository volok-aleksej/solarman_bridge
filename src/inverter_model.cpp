#include "inverter_model.hpp"

#include <algorithm>
#include <cstdio>

std::string InverterClock::date() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    return buf;
}

void RegisterBank::put(uint16_t addr, const std::vector<uint16_t>& values) {
    if (regs_.size() < addr + values.size()) regs_.resize(addr + values.size());
    std::copy(values.begin(), values.end(), regs_.begin() + addr);
}

bool RegisterBlock::on_block_done(const std::vector<uint16_t>& regs, InverterReadings& out) {
    bank_.put(addr, regs);
    return !decode_ || decode_(bank_, out);
}

SettingRef InverterModel::find_setting(const std::string& name) const {
    for (const SettingGroup& g : setting_groups)
        for (const SettingDef& d : g.defs)
            if (name == d.id) return SettingRef{&g, &d};
    return {};
}

namespace {

// Filled by the registrars during static initialisation; a function-local
// static, so it exists before the first of them runs.
std::vector<const InverterModel*>& registry() {
    static std::vector<const InverterModel*> models;
    return models;
}

}  // namespace

ModelRegistrar::ModelRegistrar(const InverterModel& model) {
    registry().push_back(&model);
}

const std::vector<const InverterModel*>& all_models() { return registry(); }

const InverterModel* find_model(const std::string& name) {
    for (const InverterModel* m : registry())
        if (name == m->name) return m;
    return nullptr;
}
