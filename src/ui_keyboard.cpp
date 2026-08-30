#include "ui_keyboard.h"

namespace {

lv_obj_t* g_keyboard = nullptr;

void hideKeyboard() {
    lv_obj_t* ta = lv_keyboard_get_textarea(g_keyboard);
    if (ta != nullptr) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(g_keyboard, nullptr);
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void keyboardEventCb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        hideKeyboard();
    }
}

void textareaFocusCb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(g_keyboard, ta);
    lv_obj_clear_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_keyboard);
}

}  // namespace

namespace UiKeyboard {

void init(lv_obj_t* rootLayer) {
    g_keyboard = lv_keyboard_create(rootLayer);
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_keyboard, keyboardEventCb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(g_keyboard, keyboardEventCb, LV_EVENT_CANCEL, nullptr);
}

void bindFocus(lv_obj_t* textarea) {
    lv_obj_add_event_cb(textarea, textareaFocusCb, LV_EVENT_FOCUSED, nullptr);
}

void hide() {
    if (g_keyboard == nullptr || lv_obj_has_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN)) return;
    hideKeyboard();
}

}  // namespace UiKeyboard
