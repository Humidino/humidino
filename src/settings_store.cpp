#include "settings_store.h"

#include <Preferences.h>
#include <cstring>

#include "config.h"

namespace {
Preferences g_prefs;
constexpr const char* kNamespace = "humidino";
}  // namespace

namespace Settings {

void begin() {
    // begin()/end() вокруг каждого load/save ниже вместо удержания
    // пространства имён открытым — Preferences не рассчитан на совместное
    // использование из нескольких задач одновременно, а настройки
    // читаются/пишутся редко.
}

RuntimeSettings load() {
    RuntimeSettings s;
    g_prefs.begin(kNamespace, true);
    s.rhTargetPercent = g_prefs.getFloat("rh_target", DEFAULT_RH_TARGET_PERCENT);
    s.hysteresisPercent = g_prefs.getFloat("hyst_pct", DEFAULT_HYSTERESIS_PERCENT);
    s.freezeProtectC = g_prefs.getFloat("freeze_c", FREEZE_PROTECT_TEMP_C);
    s.minRuntimeMs = g_prefs.getULong("min_run_ms", MIN_RUNTIME_MS);
    s.minPauseMs = g_prefs.getULong("min_pause_ms", MIN_PAUSE_MS);
    g_prefs.end();
    return s;
}

void save(const RuntimeSettings& settings) {
    RuntimeSettings current = load();

    g_prefs.begin(kNamespace, false);
    if (current.rhTargetPercent != settings.rhTargetPercent)
        g_prefs.putFloat("rh_target", settings.rhTargetPercent);
    if (current.hysteresisPercent != settings.hysteresisPercent)
        g_prefs.putFloat("hyst_pct", settings.hysteresisPercent);
    if (current.freezeProtectC != settings.freezeProtectC)
        g_prefs.putFloat("freeze_c", settings.freezeProtectC);
    if (current.minRuntimeMs != settings.minRuntimeMs)
        g_prefs.putULong("min_run_ms", settings.minRuntimeMs);
    if (current.minPauseMs != settings.minPauseMs)
        g_prefs.putULong("min_pause_ms", settings.minPauseMs);
    g_prefs.end();
}

NetConfig loadNet() {
    NetConfig n;
    g_prefs.begin(kNamespace, true);
    g_prefs.getString("mqtt_host", n.mqttHost, sizeof(n.mqttHost));
    n.mqttPort = g_prefs.getUShort("mqtt_port", DEFAULT_MQTT_PORT);
    g_prefs.getString("mqtt_user", n.mqttUser, sizeof(n.mqttUser));
    g_prefs.getString("mqtt_pass", n.mqttPass, sizeof(n.mqttPass));
    String topic = g_prefs.getString("mqtt_topic", DEFAULT_MQTT_BASE_TOPIC);
    strncpy(n.mqttBaseTopic, topic.c_str(), sizeof(n.mqttBaseTopic) - 1);
    g_prefs.end();
    return n;
}

void saveNet(const NetConfig& net) {
    g_prefs.begin(kNamespace, false);
    g_prefs.putString("mqtt_host", net.mqttHost);
    g_prefs.putUShort("mqtt_port", net.mqttPort);
    g_prefs.putString("mqtt_user", net.mqttUser);
    g_prefs.putString("mqtt_pass", net.mqttPass);
    g_prefs.putString("mqtt_topic", net.mqttBaseTopic);
    g_prefs.end();
}

}  // namespace Settings
