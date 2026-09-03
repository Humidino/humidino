#include "time_sync.h"

#include <Arduino.h>
#include <ctime>

namespace {

bool g_started = false;

// Порог, ниже которого системное время считается ещё не синхронизированным.
// Это не сама эпоха 0 — до первого успешного ответа SNTP time() возвращает
// время около 1 Jan 1970, а не ровно 0, поэтому сравниваем с заведомо более
// поздней датой, а не проверяем на ноль.
constexpr time_t kMinValidEpoch = 1700000000;  // ~14 ноября 2023

}  // namespace

namespace TimeSync {

void begin() {
    if (g_started) return;
    g_started = true;
    // UTC, без смещения часового пояса — локальное время считает уже
    // клиент (веб-браузер по своим часам) или экран платы (см.
    // LOCAL_TZ_OFFSET_SEC в config.h, используется только для группировки
    // "сегодня" в статистике).
    configTime(0, 0, "pool.ntp.org", "time.google.com");
}

bool isSynced() {
    return time(nullptr) >= kMinValidEpoch;
}

uint32_t nowEpoch() {
    time_t t = time(nullptr);
    return t >= kMinValidEpoch ? static_cast<uint32_t>(t) : 0;
}

}  // namespace TimeSync
