#include "sensors.h"

#include <Adafruit_SHT31.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "climate_math.h"
#include "config.h"
#include "i2c_recovery.h"
#include "shared_state.h"
#include "soft_i2c.h"
#include "watchdog.h"

namespace {

// ----------------------------------------------------------------------
// Абстракция над транспортом I2C: 2 датчика сидят на аппаратных контроллерах
// ESP32-S3 (Wire/Wire1), а ещё 2 — на программных bit-bang шинах (SoftI2C),
// потому что аппаратных контроллеров I2C у чипа только два (см. комментарий
// в config.h над PIN_I2C2_SDA). Остальному коду ниже (initBuses/pollOneSensor
// /recoverBusFor) без разницы, какой именно транспорт у конкретного датчика.
// ----------------------------------------------------------------------
class ISensorBus {
public:
    virtual ~ISensorBus() = default;
    virtual void begin() = 0;
    virtual bool probe(uint8_t addr) = 0;         // используется только диагностическим сканом при старте
    virtual void initSensor(uint8_t addr) = 0;
    virtual bool read(uint8_t addr, float& tempC, float& rh) = 0;
    virtual bool recover(uint8_t addr) = 0;
    virtual const char* label() const = 0;
};

class HardwareSensorBus : public ISensorBus {
public:
    HardwareSensorBus(TwoWire& wire, uint8_t sdaPin, uint8_t sclPin, uint32_t clockHz, const char* label)
        : wire_(wire), sht_(&wire), sdaPin_(sdaPin), sclPin_(sclPin), clockHz_(clockHz), label_(label) {}

    void begin() override {
        wire_.begin(sdaPin_, sclPin_, clockHz_);
        // Явный таймаут транзакции: без него отключённый/безподтяжечный
        // датчик может подвесить шину на неопределённое время и уронить
        // sensorTask по watchdog раньше, чем сработает штатное восстановление.
        wire_.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
    }

    bool probe(uint8_t addr) override {
        wire_.beginTransmission(addr);
        return wire_.endTransmission() == 0;
    }

    void initSensor(uint8_t addr) override { sht_.begin(addr); }

    bool read(uint8_t /*addr*/, float& tempC, float& rh) override {
        tempC = sht_.readTemperature();
        rh = sht_.readHumidity();
        return !isnan(tempC) && !isnan(rh);
    }

    bool recover(uint8_t addr) override {
        bool freed = I2CRecovery::recoverBus(wire_, sdaPin_, sclPin_, clockHz_);
        sht_.begin(addr);
        return freed;
    }

    const char* label() const override { return label_; }

private:
    TwoWire& wire_;
    Adafruit_SHT31 sht_;
    uint8_t sdaPin_;
    uint8_t sclPin_;
    uint32_t clockHz_;
    const char* label_;
};

// CRC-8 по датащиту Sensirion SHT3x: полином 0x31, старт 0xFF — используется
// для проверки обоих 16-битных слов (температура, влажность) в ответе.
uint8_t sht31Crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

class SoftwareSensorBus : public ISensorBus {
public:
    SoftwareSensorBus(uint8_t sdaPin, uint8_t sclPin, uint32_t clockHz, const char* label)
        : sdaPin_(sdaPin), sclPin_(sclPin), clockHz_(clockHz), label_(label) {}

    void begin() override { i2c_.begin(sdaPin_, sclPin_, clockHz_); }

    bool probe(uint8_t addr) override { return i2c_.writeBytes(addr, nullptr, 0); }

    // Команде single-shot ниже не нужна отдельная инициализация датчика —
    // в отличие от Adafruit_SHT31::begin() (используется только на
    // аппаратных шинах), здесь просто нечего настраивать заранее.
    void initSensor(uint8_t /*addr*/) override {}

    bool read(uint8_t addr, float& tempC, float& rh) override {
        // 0x2400 — единичное измерение, high repeatability, БЕЗ растяжки
        // такта (в отличие от 0x2C.. с clock stretching): такой выбор
        // избавляет bit-bang от необходимости держать SCL низко удерживаемым
        // ведомым все ~15 мс измерения — вместо этого мы сами ждём delay()
        // ниже и потом читаем результат обычным чтением.
        static const uint8_t cmd[2] = {0x24, 0x00};
        if (!i2c_.writeBytes(addr, cmd, sizeof(cmd))) return false;
        delay(16);  // макс. время измерения при high repeatability по даташиту SHT3x — 15 мс, +1 мс запас
        uint8_t buf[6];
        if (!i2c_.readBytes(addr, buf, sizeof(buf))) return false;
        if (sht31Crc8(buf, 2) != buf[2] || sht31Crc8(buf + 3, 2) != buf[5]) return false;

        uint16_t rawT = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
        uint16_t rawRh = static_cast<uint16_t>((buf[3] << 8) | buf[4]);
        tempC = -45.0f + 175.0f * (static_cast<float>(rawT) / 65535.0f);
        rh = 100.0f * (static_cast<float>(rawRh) / 65535.0f);
        return true;
    }

    bool recover(uint8_t /*addr*/) override {
        bool freed = I2CRecovery::unstickPins(sdaPin_, sclPin_);
        i2c_.begin(sdaPin_, sclPin_, clockHz_);
        return freed;
    }

    const char* label() const override { return label_; }

private:
    SoftI2C i2c_;
    uint8_t sdaPin_;
    uint8_t sclPin_;
    uint32_t clockHz_;
    const char* label_;
};

HardwareSensorBus g_bus0(Wire, PIN_I2C0_SDA, PIN_I2C0_SCL, I2C_CLOCK_HZ, "Wire (I2C0, приточка)");
SoftwareSensorBus g_bus2(PIN_I2C2_SDA, PIN_I2C2_SCL, SOFT_I2C_CLOCK_HZ, "SoftI2C (I2C2, середина)");
SoftwareSensorBus g_bus3(PIN_I2C3_SDA, PIN_I2C3_SCL, SOFT_I2C_CLOCK_HZ, "SoftI2C (I2C3, дальний угол)");
HardwareSensorBus g_bus1(Wire1, PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_CLOCK_HZ, "Wire1 (I2C1, улица)");

// ----------------------------------------------------------------------
// Кольцевой буфер скользящего среднего на каждый датчик (сырые значения не
// покидают этот файл — в ShaState публикуется только отфильтрованный
// результат).
// ----------------------------------------------------------------------
struct SensorFilter {
    float tempBuf[MOVING_AVG_WINDOW] = {};
    float rhBuf[MOVING_AVG_WINDOW] = {};
    uint8_t idx = 0;
    uint8_t count = 0;

    void push(float t, float rh) {
        tempBuf[idx] = t;
        rhBuf[idx] = rh;
        idx = (idx + 1) % MOVING_AVG_WINDOW;
        if (count < MOVING_AVG_WINDOW) count++;
    }

    float avgTemp() const { return average(tempBuf); }
    float avgRh() const { return average(rhBuf); }

private:
    float average(const float* buf) const {
        if (count == 0) return NAN;
        float sum = 0;
        for (uint8_t i = 0; i < count; i++) sum += buf[i];
        return sum / count;
    }
};

struct SensorContext {
    SensorId id;
    ISensorBus* bus;  // у каждого датчика своя шина — 1 датчик на 1 шину, ни одна не делится
    uint8_t addr;
    SensorFilter filter;
    uint8_t consecutiveFailures = 0;
    // Датчик, который не отвечает даже после восстановления шины (не
    // подключён физически), не должен опрашиваться на каждом цикле — иначе
    // его таймауты замедляют/блокируют опрос остальных датчиков. offline=true
    // снимает его с обычного темпа опроса до nextRetryMs, когда даётся редкая
    // попытка проверить, не появился ли он снова.
    bool offline = false;
    uint32_t nextRetryMs = 0;
};

SensorContext g_sensors[] = {
    {SensorId::CrawlspaceIntake, &g_bus0, SHT31_ADDR_A},
    {SensorId::CrawlspaceMid,    &g_bus2, SHT31_ADDR_A},
    {SensorId::CrawlspaceFar,    &g_bus3, SHT31_ADDR_A},
    {SensorId::Outside,          &g_bus1, SHT31_ADDR_A},
};
constexpr size_t kSensorCount = sizeof(g_sensors) / sizeof(g_sensors[0]);

// Сканирует шину и печатает найденные адреса в Serial — единственный способ
// на месте отличить "физически ничего не отвечает" (обрыв/питание/подтяжки)
// от "отвечает не на том адресе" (ADDR разведён не так, как ждёт прошивка).
// Проверяем только реально используемые адреса SHT31 (0x44/0x45), а не все
// 127 — на неподключённой/безподтяжечной шине каждая транзакция может
// целиком съедать таймаут, и полный перебор адресов на всех четырёх шинах
// превращает старт платы в многоминутное ожидание ещё до того, как
// опросится хоть один реально подключённый датчик.
void scanBus(ISensorBus& bus) {
    Serial.printf("I2C scan %s: ", bus.label());
    bool found = false;
    for (uint8_t addr : {SHT31_ADDR_A, SHT31_ADDR_B}) {
        if (bus.probe(addr)) {
            Serial.printf("0x%02X ", addr);
            found = true;
        }
        Watchdog::feed();
    }
    if (!found) Serial.print("(нет ответов)");
    Serial.println();
}

void initBuses() {
    g_bus0.begin();
    g_bus1.begin();
    g_bus2.begin();
    g_bus3.begin();

    scanBus(g_bus0);
    scanBus(g_bus1);
    scanBus(g_bus2);
    scanBus(g_bus3);

    for (auto& ctx : g_sensors) ctx.bus->initSensor(ctx.addr);
}

// Восстанавливает шину датчика и заново инициализирует сам датчик на ней.
// В отличие от более ранней версии прошивки, здесь не нужно перебирать
// "соседей по шине" — у каждого датчика теперь своя отдельная шина, и сброс
// одной шины не может задеть остальные три.
void recoverBusFor(SensorContext& ctx) {
    ctx.bus->recover(ctx.addr);
    ctx.consecutiveFailures = 0;
}

// Снимает датчик с обычного темпа опроса на SENSOR_OFFLINE_RETRY_MS —
// вызывается, когда он не отвечает даже сразу после recoverBusFor. Без
// этого физически не подключённый датчик продолжал бы конкурировать за
// свою шину на каждом цикле опроса без всякого смысла.
void markOffline(SensorContext& ctx) {
    ctx.offline = true;
    ctx.nextRetryMs = millis() + SENSOR_OFFLINE_RETRY_MS;
}

void pollOneSensor(SensorContext& ctx) {
    float t = NAN, rh = NAN;
    bool ok = ctx.bus->read(ctx.addr, t, rh);

    SensorReading reading;

    if (ok) {
        ctx.consecutiveFailures = 0;
        ctx.offline = false;
        ctx.filter.push(t, rh);
        float filteredT = ctx.filter.avgTemp();
        float filteredRh = ctx.filter.avgRh();

        reading.temperatureC = filteredT;
        reading.humidityPct = filteredRh;
        reading.dewPointC = ClimateMath::dewPointC(filteredT, filteredRh);
        reading.absHumidityGm3 = ClimateMath::absHumidityGm3(filteredT, filteredRh);
        reading.valid = true;
        reading.error = false;
    } else {
        ctx.consecutiveFailures++;
        Serial.printf("Sensor %d (addr 0x%02X) poll failed, streak=%u\n",
                      static_cast<int>(ctx.id), ctx.addr, ctx.consecutiveFailures);
        // Оставляем на дисплее последние известные отфильтрованные значения,
        // просто помечая ошибку — единичный сбой не должен обнулять панель.
        SystemState snapshot;
        if (ShaState::getSnapshot(snapshot)) {
            reading = snapshot.readings[static_cast<size_t>(ctx.id)];
        }
        reading.error = true;

        if (ctx.consecutiveFailures >= I2C_FAILURE_RECOVERY_THRESHOLD) {
            recoverBusFor(ctx);
            // Один быстрый повтор сразу после восстановления шины — если
            // датчик и теперь не отвечает, скорее всего он просто не
            // подключён физически, и продолжать долбить шину каждые
            // SENSOR_POLL_INTERVAL_MS смысла нет.
            float retryT, retryRh;
            if (!ctx.bus->read(ctx.addr, retryT, retryRh)) {
                markOffline(ctx);
            } else {
                ctx.consecutiveFailures = 0;
            }
        }
    }
    reading.lastUpdateMs = millis();
    ShaState::updateSensor(ctx.id, reading);
}

void sensorTask(void*) {
    Watchdog::registerCurrentTask();
    initBuses();

    for (;;) {
        uint32_t now = millis();
        for (auto& ctx : g_sensors) {
            if (ctx.offline && now < ctx.nextRetryMs) {
                // Молчащий датчик пропускаем без единой I2C-транзакции —
                // иначе даже редкие таймауты на его адресе тормозили бы
                // опрос остальных датчиков.
                Watchdog::feed();
                continue;
            }
            pollOneSensor(ctx);
            if (ctx.offline) {
                // Датчик остался офлайн после этой попытки (будь то плановый
                // повтор раз в SENSOR_OFFLINE_RETRY_MS или ещё не дошедший до
                // markOffline() свежий сбой) — сдвигаем окно следующей
                // попытки вперёд. Без этого при уже истёкшем старом
                // nextRetryMs датчик опрашивался бы на каждом цикле, а не
                // раз в SENSOR_OFFLINE_RETRY_MS.
                ctx.nextRetryMs = now + SENSOR_OFFLINE_RETRY_MS;
            }
            // Кормим watchdog после каждого датчика, а не только в конце
            // прохода по всем четырём — иначе время ожидания зависшего/
            // отключённого датчика на одной шине суммируется с остальными и
            // может выбить WDT_TIMEOUT_S раньше, чем найдётся виновник.
            Watchdog::feed();
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }
}

static_assert(kSensorCount == static_cast<size_t>(SensorId::Count), "g_sensors must cover every SensorId");

}  // namespace

namespace Sensors {

void begin() {
    xTaskCreatePinnedToCore(sensorTask, "sensorTask", 4096, nullptr, 3, nullptr, 0);
}

}  // namespace Sensors
