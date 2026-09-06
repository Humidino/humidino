#pragma once
inline void vTaskDelay(unsigned) {}
inline void xTaskCreatePinnedToCore(void (*)(void*), const char*, unsigned, void*, unsigned, void*, unsigned) {}
