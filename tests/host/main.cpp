#include <cstdlib>
#include <cstring>
#include <iostream>

// Exercise the actual controller and widgets; only hardware/storage are stubbed.
#include "relay.cpp"
#include "ui_dashboard.cpp"
#include "ui_settings_screen.cpp"

SystemState testState;
RuntimeSettings testSaved;
namespace ShaState {
RuntimeSettings getSettings() { return testState.settings; }
bool getSnapshot(SystemState& out) { out = testState; return true; }
void updateSettings(const RuntimeSettings& s) { testState.settings = s; }
void updateRelay(const RelayStatus& s) { testState.relay = s; }
}
namespace Settings {
void save(const RuntimeSettings& s) { testSaved = s; }
uint32_t loadCycleCount() { return 0; }
void saveCycleCount(uint32_t) {}
std::vector<Preset> loadPresets() { return {}; }
}
namespace Season {
Id current() { return Id::Autumn; }
Settings::PresetValues profileFor(Id) { return {}; }
}
namespace Backlight { void noteActivity() {} }
namespace Watchdog { void registerCurrentTask() {} void feed() {} }
namespace RunLog {
void recordStart(float, float, float, float) {}
void recordStop(float, float, float, float, StopReason, uint32_t) {}
}

void check(bool condition, const char* description) {
    if (!condition) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
    std::cout << "PASS: " << description << '\n';
}

void healthyReadings() {
    for (auto& r : testState.readings) {
        r.valid = true; r.error = false;
        r.temperatureC = 20; r.humidityPct = 80; r.absHumidityGm3 = 12;
    }
    testState.readings[static_cast<size_t>(SensorId::Outside)].absHumidityGm3 = 8;
}

int main() {
    healthyReadings();
    RelayController controller;
    controller.begin(testState.settings.minPauseMs);
    auto evaluate = [&] {
        testNow += 1000;
        controller.update(testState.readings, testState.settings, testNow);
        testState.relay = controller.status();
    };
    testState.settings.mode = OperatingMode::ManualOn;
    evaluate();
    check(testState.relay.relayOn && testRelayLevel == HIGH, "manual on drives GPIO high");
    testState.settings.mode = OperatingMode::ManualOff;
    evaluate();
    check(!testState.relay.relayOn && testRelayLevel == LOW, "manual off ignores minimum runtime");
    testState.settings.mode = OperatingMode::ManualOn;
    testState.readings[3].absHumidityGm3 = 20;
    evaluate();
    check(testState.relay.relayOn, "manual on ignores pause and condensation");
    testState.readings[3].temperatureC = testState.settings.freezeProtectC;
    evaluate();
    check(testState.relay.state == RelayControlState::LockedOutFreeze && testRelayLevel == LOW,
          "freeze protection stops manual operation");
    healthyReadings(); testState.readings[3].error = true;
    evaluate();
    check(testState.relay.state == RelayControlState::LockedOutSensorFault, "outside failure blocks manual on");
    testState.settings.mode = OperatingMode::ManualOff;
    evaluate();
    check(testState.relay.state == RelayControlState::Idle && testRelayLevel == LOW,
          "manual off works with failed sensors");
    healthyReadings(); testState.settings.mode = OperatingMode::Auto;
    testNow += testState.settings.minPauseMs;
    evaluate();
    check(testState.relay.relayOn, "auto starts above humidity target after pause");
    for (size_t i = 0; i < 3; ++i) testState.readings[i].humidityPct = 50;
    evaluate();
    check(testState.relay.relayOn, "auto respects minimum runtime");
    testNow += testState.settings.minRuntimeMs;
    evaluate();
    check(!testState.relay.relayOn, "auto stops below hysteresis after minimum runtime");
    healthyReadings(); evaluate();
    check(testState.relay.state == RelayControlState::MinPauseHold, "auto respects minimum pause");
    testState.settings.mode = OperatingMode::ManualOn;
    testState.readings[0].error = true; testState.readings[1].error = true;
    evaluate();
    check(testState.relay.relayOn && testState.relay.crawlspaceLiveSensors == 1,
          "one remaining crawlspace sensor permits operation");
    testState.readings[2].error = true; evaluate();
    check(!testState.relay.relayOn, "loss of all crawlspace sensors stops relay");

    healthyReadings();
    RuntimeSettings lockoutSettings = testState.settings;
    lockoutSettings.mode = OperatingMode::ManualOn;
    lockoutSettings.maxRuntimeMs = 2000;
    lockoutSettings.minPauseMs = 5000;
    RelayController lockoutController;
    lockoutController.begin(lockoutSettings.minPauseMs);
    auto evaluateLockout = [&](uint32_t advanceMs) {
        testNow += advanceMs;
        lockoutController.update(testState.readings, lockoutSettings, testNow);
    };
    evaluateLockout(1000);
    check(lockoutController.status().relayOn, "max-runtime scenario starts in manual on");
    evaluateLockout(lockoutSettings.maxRuntimeMs);
    check(lockoutController.status().state == RelayControlState::LockedOutMaxRuntime,
          "max runtime stops manual operation");
    lockoutSettings.mode = OperatingMode::ManualOff;
    evaluateLockout(1000);
    check(lockoutController.status().state == RelayControlState::Idle,
          "manual off may display idle during max-runtime pause");
    lockoutSettings.mode = OperatingMode::ManualOn;
    evaluateLockout(1000);
    check(!lockoutController.status().relayOn, "manual on cannot bypass max-runtime pause after manual off");
    lockoutSettings.mode = OperatingMode::Auto;
    for (size_t i = 0; i < 3; ++i) testState.readings[i].humidityPct = 50;
    evaluateLockout(1000);
    check(lockoutController.status().state == RelayControlState::Idle,
          "automatic humidity control may return max-runtime lockout to idle");
    lockoutSettings.mode = OperatingMode::ManualOn;
    evaluateLockout(1000);
    check(!lockoutController.status().relayOn, "manual on cannot bypass max-runtime pause after auto idle");
    evaluateLockout(1000);
    check(lockoutController.status().relayOn, "manual on starts when max-runtime pause expires");

    lv_init();
    lv_display_t* display = lv_display_create(480, 320);
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_set_size(parent, 480, 280);
    lv_obj_set_style_border_width(parent, 0, 0);
    UiDashboard::build(parent);
    lv_obj_update_layout(parent);
    check(lv_obj_get_height(g_banner) >= 78, "dashboard banner has room for all status rows");
    for (auto* btn : g_modeButtons) {
        auto* row = lv_obj_get_parent(btn);
        lv_area_t buttonArea, rowArea;
        lv_obj_get_coords(btn, &buttonArea); lv_obj_get_coords(row, &rowArea);
        std::cout << "button height=" << lv_obj_get_height(btn)
                  << " row height=" << lv_obj_get_height(row) << '\n';
        check(buttonArea.y1 >= rowArea.y1 && buttonArea.y2 <= rowArea.y2,
              "mode button fits visible row");
    }
    healthyReadings();
    testState.settings.mode = OperatingMode::Auto;
    for (OperatingMode mode : {OperatingMode::ManualOn, OperatingMode::ManualOff, OperatingMode::Auto}) {
        static lv_point_t touchPoint;
        static bool touchPressed = false;
        static lv_indev_t* touch = lv_indev_create();
        lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(touch, display);
        lv_indev_set_read_cb(touch, [](lv_indev_t*, lv_indev_data_t* data) {
            data->point = touchPoint;
            data->state = touchPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        });
        lv_obj_update_layout(parent);
        lv_area_t area;
        lv_obj_get_coords(g_modeButtons[static_cast<size_t>(mode)], &area);
        touchPoint = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
        touchPressed = true; lv_indev_read(touch);
        touchPressed = false; lv_indev_read(touch);
        check(testState.settings.mode == mode && testSaved.mode == mode, "button updates and saves selected mode");
        evaluate(); UiDashboard::update();
        if (mode == OperatingMode::ManualOn) check(testRelayLevel == HIGH, "ON button reaches relay");
        if (mode == OperatingMode::ManualOff) check(testRelayLevel == LOW, "OFF button reaches relay");
    }
    testState.relay.state = RelayControlState::LockedOutSensorFault;
    testState.relay.relayOn = false;
    UiDashboard::update();
    check(std::strcmp(lv_label_get_text(g_bannerLabel), "ВЫКЛ: ошибка датчиков") == 0,
          "dashboard explains blocked start");
    lv_obj_clean(parent);
    UiSettingsScreen::build(parent);
    lv_obj_update_layout(parent);
    auto* footer = lv_obj_get_child(parent, -1);
    auto* save = lv_obj_get_child(footer, 0);
    lv_area_t footerArea, saveArea;
    lv_obj_get_coords(footer, &footerArea); lv_obj_get_coords(save, &saveArea);
    check(saveArea.y1 >= footerArea.y1 && saveArea.y2 <= footerArea.y2,
          "settings save button fits footer");
    const int32_t initialTarget = g_rows[kRhTarget].value;
    auto* targetRow = lv_obj_get_child(parent, 1);
    lv_obj_send_event(lv_obj_get_child(targetRow, 3), LV_EVENT_SHORT_CLICKED, nullptr);
    check(g_rows[kRhTarget].value == initialTarget + 1, "settings plus changes value");
    lv_obj_send_event(save, LV_EVENT_CLICKED, nullptr);
    check(testSaved.rhTargetPercent == initialTarget + 1, "settings save persists edited value");
    lv_obj_send_event(lv_obj_get_child(targetRow, 1), LV_EVENT_SHORT_CLICKED, nullptr);
    check(g_rows[kRhTarget].value == initialTarget, "settings minus changes value");
    lv_display_delete(display);
    std::cout << "All firmware checks passed\n";
}
