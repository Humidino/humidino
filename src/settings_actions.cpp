#include "settings_actions.h"

#include <cstring>

#include "mqtt.h"

namespace SettingsActions {

void applyRuntimeSettings(const RuntimeSettings& settings) {
    ShaState::updateSettings(settings);
    Settings::save(settings);
}

bool applyPresetByName(const char* name, RuntimeSettings& outApplied) {
    std::vector<Settings::Preset> presets = Settings::loadPresets();
    const Settings::Preset* found = nullptr;
    for (const auto& p : presets) {
        if (strcmp(p.name, name) == 0) {
            found = &p;
            break;
        }
    }
    if (found == nullptr) return false;

    RuntimeSettings settings = ShaState::getSettings();
    settings.rhTargetPercent = found->values.rhTargetPercent;
    settings.hysteresisPercent = found->values.hysteresisPercent;
    settings.freezeProtectC = found->values.freezeProtectC;
    settings.minRuntimeMs = found->values.minRuntimeMs;
    settings.minPauseMs = found->values.minPauseMs;

    applyRuntimeSettings(settings);
    outApplied = settings;
    return true;
}

void saveNetworkConfig(const Settings::NetConfig& net) {
    Settings::saveNet(net);
    Mqtt::reconfigure();
}

}  // namespace SettingsActions
