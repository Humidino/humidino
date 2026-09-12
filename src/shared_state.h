#pragma once
#include <cmath>
#include <cstdint>

#include "config.h"

// ============================================================================
// Модель данных, общая для sensorTask, controlTask, lvglTask и netTask.
// Любой доступ идёт только через ShaState::getSnapshot()/update* ниже —
// напрямую трогать структуру из более чем одной задачи нельзя.
// ============================================================================

struct SensorReading {
    float temperatureC = NAN;
    float humidityPct = NAN;
    float dewPointC = NAN;
    float absHumidityGm3 = NAN;  // г/м³
    bool valid = false;   // true после хотя бы одного успешного измерения
    bool error = false;   // true, если последний опрос не удался (красный крест на дашборде)
    uint32_t lastUpdateMs = 0;   // millis() на момент последнего обновления
};

enum class RelayControlState : uint8_t {
    Idle,
    Running,
    LockedOutCondensation,
    LockedOutFreeze,
    MinPauseHold,
    LockedOutSensorFault,
    // Добавлено в конец — значение используется как числовой код в JSON-API
    // (см. Telemetry::buildStateJson), существующие коды менять нельзя.
    LockedOutMaxRuntime
};

// Режим управления вентиляцией, выбирается пользователем (веб-интерфейс —
// экран платы без тачскрина, только отображает текущий режим).
enum class OperatingMode : uint8_t {
    Auto,       // текущий алгоритм по порогам влажности/точки росы
    ManualOn,   // принудительно включено (защита по морозу всё равно активна)
    ManualOff,  // принудительно выключено
};

const char* toString(OperatingMode mode);
// Возвращает Auto, если строка не распознана — безопасное умолчание.
OperatingMode operatingModeFromString(const char* s);

// Стабильный машиночитаемый идентификатор состояния реле (для JSON-API —
// человекочитаемый текст на русском см. Relay::bannerText в relay.h).
const char* toString(RelayControlState state);

struct RelayStatus {
    RelayControlState state = RelayControlState::Idle;
    bool relayOn = false;
    uint32_t stateEnteredMs = 0;
    uint32_t lastOnMs = 0;
    uint32_t lastOffMs = 0;
    // Сколько раз реле включалось за всё время жизни устройства (не за
    // текущую сессию) — переживает перезагрузки, см. Settings::loadCycleCount()
    // в settings_store.h.
    uint32_t cycleCount = 0;
    // Сколько из CRAWLSPACE_SENSOR_COUNT датчиков подпола сейчас дают
    // валидные показания без ошибки (см. relay.cpp::summarizeCrawlspace).
    // 0 => LockedOutSensorFault (нечем управлять); меньше CRAWLSPACE_SENSOR_COUNT,
    // но больше 0 => реле продолжает работать по оставшимся живым датчикам
    // (деградированный режим, не блокировка).
    uint8_t crawlspaceLiveSensors = 0;
    // Агрегированная влажность подпола (максимум среди живых датчиков — тот
    // же показатель, что участвует в пороговых решениях, см.
    // relay.cpp::summarizeCrawlspace) на момент последнего опроса controlTask.
    // NAN, если сейчас нет ни одного живого датчика подпола.
    float crawlspaceRhPercent = NAN;
    // Значение crawlspaceRhPercent в момент, когда реле включилось в текущем
    // цикле работы — используется, чтобы показать, насколько влажность упала
    // с начала текущего запуска (dashboard/веб). NAN, если реле ни разу не
    // включалось после старта устройства.
    float runStartCrawlRhPercent = NAN;
};

struct RuntimeSettings {
    float rhTargetPercent = DEFAULT_RH_TARGET_PERCENT;
    float hysteresisPercent = DEFAULT_HYSTERESIS_PERCENT;
    float freezeProtectC = FREEZE_PROTECT_TEMP_C;
    uint32_t minRuntimeMs = MIN_RUNTIME_MS;
    uint32_t minPauseMs = MIN_PAUSE_MS;
    // Аварийный потолок непрерывной работы реле, 0 = отключено — см.
    // MAX_RUNTIME_MS в config.h и RelayControlState::LockedOutMaxRuntime в
    // relay.cpp. Сознательно не входит в сезонные профили/пресеты
    // (season.cpp, PresetValues) — это не порог осушения, а независимая
    // защита, которая не должна меняться при смене сезона или пресета.
    uint32_t maxRuntimeMs = MAX_RUNTIME_MS;
    OperatingMode mode = OperatingMode::Auto;
    // Если true — фоновая задача Season (см. season.h) сама подставляет сюда
    // пороги/тайминги текущего календарного сезона (профили подобраны под
    // климат Лотошино, МО, см. docs/SEASONAL_LOTOSHINO.md) при каждой смене
    // сезона. Ручное редактирование полей выше при этом не запрещено, но
    // будет перезаписано на следующей смене сезона — это ортогональный
    // "автопилот", а не разовая настройка. По умолчанию включён.
    bool seasonAutoEnabled = true;
};

struct SystemState {
    SensorReading readings[static_cast<size_t>(SensorId::Count)];
    RelayStatus relay;
    RuntimeSettings settings;
    bool wifiConnected = false;
    int8_t wifiRssi = 0;
    // Локальный IPv4, полученный по DHCP от точки доступа; пустая строка,
    // пока нет подключения. Показывается на дашборде, чтобы можно было
    // открыть локальный веб-интерфейс, не заходя в роутер за списком клиентов.
    char wifiIp[16] = "";
};

// Доступ под мьютексом. Каждый вызов берёт блокировку с коротким таймаутом,
// чтобы зависшая операция I2C/SPI/сети в одной задаче не могла превратиться
// в общесистемное зависание другой задачи.
namespace ShaState {

void begin();

// Копирует всё состояние под блокировкой. Возвращает false (не трогая
// `out`), если не удалось взять блокировку за отведённый таймаут.
bool getSnapshot(SystemState& out);

void updateSensor(SensorId id, const SensorReading& reading);
void updateRelay(const RelayStatus& status);
void updateSettings(const RuntimeSettings& settings);
// ip — локальный IPv4 в виде строки ("192.168.1.23") или nullptr/"", если
// подключения нет.
void updateWifi(bool connected, int8_t rssi, const char* ip = nullptr);

// Вспомогательная функция для вызывающих, которым нужно только одно поле —
// не копирует всю структуру целиком.
RuntimeSettings getSettings();

}  // namespace ShaState
