#include "ui_network_screen.h"

#include <Arduino.h>
#include <cstdlib>
#include <cstring>

#include "fonts/fonts.h"
#include "settings_actions.h"
#include "settings_store.h"
#include "ui_keyboard.h"

namespace {

lv_obj_t* g_hostField;
lv_obj_t* g_portField;
lv_obj_t* g_portError;
lv_obj_t* g_userField;
lv_obj_t* g_passField;
lv_obj_t* g_passHint;
lv_obj_t* g_topicField;
lv_obj_t* g_savedFlash;
lv_timer_t* g_flashTimer = nullptr;

lv_obj_t* buildTextField(lv_obj_t* parent, const char* labelText, uint32_t maxLen,
                          const char* placeholder, bool password) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &font_ru_14, 0);
    lv_label_set_text(lbl, labelText);

    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, 40);
    lv_obj_set_style_text_font(ta, &font_ru_14, 0);  // иначе плейсхолдер на кириллице не отрисуется
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, maxLen);
    if (placeholder != nullptr) lv_textarea_set_placeholder_text(ta, placeholder);
    if (password) lv_textarea_set_password_mode(ta, true);
    UiKeyboard::bindFocus(ta);
    return ta;
}

void hideFlash(lv_timer_t*) {
    lv_obj_add_flag(g_savedFlash, LV_OBJ_FLAG_HIDDEN);
    g_flashTimer = nullptr;
}

void showSavedFlash() {
    lv_obj_clear_flag(g_savedFlash, LV_OBJ_FLAG_HIDDEN);
    if (g_flashTimer != nullptr) lv_timer_reset(g_flashTimer);
    else g_flashTimer = lv_timer_create(hideFlash, 1500, nullptr);
    lv_timer_set_repeat_count(g_flashTimer, 1);
}

void onSaveClicked(lv_event_t*) {
    // Читаем сохранённый пароль отдельно — поле на экране его никогда не
    // содержит (см. build()), пустое поле означает "не менять", как и в
    // POST /api/network.
    Settings::NetConfig net = Settings::loadNet();

    const char* host = lv_textarea_get_text(g_hostField);
    strncpy(net.mqttHost, host, sizeof(net.mqttHost) - 1);

    const char* portText = lv_textarea_get_text(g_portField);
    long parsed = portText != nullptr ? strtol(portText, nullptr, 10) : 0;
    if (parsed <= 0 || parsed > 65535) {
        lv_label_set_text(g_portError, "Порт должен быть от 1 до 65535");
        return;
    }
    lv_label_set_text(g_portError, "");
    net.mqttPort = static_cast<uint16_t>(parsed);

    const char* user = lv_textarea_get_text(g_userField);
    strncpy(net.mqttUser, user, sizeof(net.mqttUser) - 1);

    const char* pass = lv_textarea_get_text(g_passField);
    if (pass != nullptr && strlen(pass) > 0) {
        strncpy(net.mqttPass, pass, sizeof(net.mqttPass) - 1);
    }

    const char* topic = lv_textarea_get_text(g_topicField);
    strncpy(net.mqttBaseTopic, topic, sizeof(net.mqttBaseTopic) - 1);

    SettingsActions::saveNetworkConfig(net);

    lv_textarea_set_text(g_passField, "");  // как и в вебе, пароль на экране не задерживается
    lv_label_set_text(g_passHint, strlen(net.mqttPass) > 0 ? "Пароль задан" : "Пароль не задан");
    showSavedFlash();
}

}  // namespace

namespace UiNetworkScreen {

void build(lv_obj_t* parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x101418), 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);

    lv_obj_t* title = lv_label_create(parent);
    lv_obj_set_style_text_font(title, &font_ru_20, 0);
    lv_label_set_text(title, "MQTT-брокер");

    g_hostField = buildTextField(parent, "Хост", sizeof(Settings::NetConfig::mqttHost) - 1,
                                  "например, 192.168.1.10", false);
    g_portField = buildTextField(parent, "Порт", 5, "1883", false);
    lv_textarea_set_accepted_chars(g_portField, "0123456789");
    g_portError = lv_label_create(parent);
    lv_obj_set_style_text_font(g_portError, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_portError, lv_color_hex(0xE04040), 0);
    lv_label_set_text(g_portError, "");
    g_userField = buildTextField(parent, "Логин", sizeof(Settings::NetConfig::mqttUser) - 1, nullptr, false);
    g_passField = buildTextField(parent, "Пароль", sizeof(Settings::NetConfig::mqttPass) - 1,
                                  "оставьте пустым, чтобы не менять", true);

    g_passHint = lv_label_create(parent);
    lv_obj_set_style_text_font(g_passHint, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_passHint, lv_color_hex(0x8AA0B8), 0);
    lv_label_set_text(g_passHint, "Пароль не задан");

    g_topicField = buildTextField(parent, "Базовый топик", sizeof(Settings::NetConfig::mqttBaseTopic) - 1,
                                   "humidino", false);

    lv_obj_t* footer = lv_obj_create(parent);
    lv_obj_set_size(footer, LV_PCT(100), 44);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(footer, 12, 0);
    lv_obj_set_style_border_width(footer, 0, 0);

    lv_obj_t* saveBtn = lv_button_create(footer);
    lv_obj_add_event_cb(saveBtn, onSaveClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLbl = lv_label_create(saveBtn);
    lv_obj_set_style_text_font(saveLbl, &font_ru_14, 0);
    lv_label_set_text(saveLbl, "Сохранить");
    lv_obj_center(saveLbl);

    g_savedFlash = lv_label_create(footer);
    lv_obj_set_style_text_font(g_savedFlash, &font_ru_14, 0);
    lv_obj_set_style_text_color(g_savedFlash, lv_color_hex(0x4CAF50), 0);
    lv_label_set_text(g_savedFlash, "\xE2\x9C\x93 применено");  // "✓ применено"
    lv_obj_add_flag(g_savedFlash, LV_OBJ_FLAG_HIDDEN);

    refresh();
}

void refresh() {
    Settings::NetConfig net = Settings::loadNet();
    lv_textarea_set_text(g_hostField, net.mqttHost);

    char portBuf[8];
    snprintf(portBuf, sizeof(portBuf), "%u", net.mqttPort);
    lv_textarea_set_text(g_portField, portBuf);
    lv_label_set_text(g_portError, "");

    lv_textarea_set_text(g_userField, net.mqttUser);
    lv_textarea_set_text(g_passField, "");  // пароль никогда не подставляется в поле
    lv_label_set_text(g_passHint, strlen(net.mqttPass) > 0 ? "Пароль задан" : "Пароль не задан");
    lv_textarea_set_text(g_topicField, net.mqttBaseTopic);
}

}  // namespace UiNetworkScreen
