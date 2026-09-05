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
#include "watchdog.h"

namespace {

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
    Adafruit_SHT31 sht;
    TwoWire* bus;
    uint8_t addr;
    uint8_t sdaPin;
    uint8_t sclPin;
    uint8_t consecutiveFailures = 0;
    SensorFilter filter;
    // Датчик, который не отвечает даже после восстановления шины (не
    // подключён физически), не должен опрашиваться на каждом цикле — иначе
    // его таймауты на общей шине замедляют/блокируют опрос соседа, который
    // реально подключён. offline=true снимает его с обычного темпа опроса
    // до nextRetryMs, когда даётся редкая попытка проверить, не появился ли
    // он снова.
    bool offline = false;
    uint32_t nextRetryMs = 0;
};

// У Adafruit_SHT31::begin(addr) нет параметра TwoWire — указатель на шину
// фиксируется при конструировании, поэтому аргумент конструктора у каждого
// датчика должен совпадать с полем `bus` ниже. Топология — см. комментарии у
// PIN_I2C0_*/PIN_I2C1_* и SensorId в config.h: зоны 1 и 3 подпола делят шину0
// (разные адреса SHT31), зона 2 подпола делит шину1 с уличным датчиком.
SensorContext g_sensors[] = {
    {SensorId::CrawlspaceZone1, Adafruit_SHT31(&Wire), &Wire, SHT31_ADDR_A, PIN_I2C0_SDA, PIN_I2C0_SCL},
    {SensorId::CrawlspaceZone3, Adafruit_SHT31(&Wire), &Wire, SHT31_ADDR_B, PIN_I2C0_SDA, PIN_I2C0_SCL},
    {SensorId::Outside,         Adafruit_SHT31(&Wire1), &Wire1, SHT31_ADDR_A, PIN_I2C1_SDA, PIN_I2C1_SCL},
    {SensorId::CrawlspaceZone2, Adafruit_SHT31(&Wire1), &Wire1, SHT31_ADDR_B, PIN_I2C1_SDA, PIN_I2C1_SCL},
};
constexpr size_t kSensorCount = sizeof(g_sensors) / sizeof(g_sensors[0]);

// Сканирует шину и печатает найденные адреса в Serial — единственный способ
// на месте отличить "физически ничего не отвечает" (обрыв/питание/подтяжки)
// от "отвечает не на том адресе" (ADDR разведён не так, как ждёт прошивка).
// Проверяем только реально используемые адреса SHT31 (0x44/0x45), а не все
// 127 — на неподключённой/безподтяжечной шине каждая транзакция может
// целиком съедать I2C_TRANSACTION_TIMEOUT_MS, и полный перебор 126 адресов
// на обеих шинах превращает старт платы в многоминутное ожидание ещё до
// того, как опросится хоть один реально подключённый датчик.
void scanBus(const char* label, TwoWire& bus) {
    Serial.printf("I2C scan %s: ", label);
    bool found = false;
    for (uint8_t addr : {SHT31_ADDR_A, SHT31_ADDR_B}) {
        bus.beginTransmission(addr);
        if (bus.endTransmission() == 0) {
            Serial.printf("0x%02X ", addr);
            found = true;
        }
        Watchdog::feed();
    }
    if (!found) Serial.print("(нет ответов)");
    Serial.println();
}

void initBuses() {
    Wire.begin(PIN_I2C0_SDA, PIN_I2C0_SCL, I2C_CLOCK_HZ);
    Wire1.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, I2C_CLOCK_HZ);
    // Явный таймаут транзакции: без него отключённый/безподтяжечный датчик
    // может подвесить шину на неопределённое время и уронить sensorTask по
    // watchdog раньше, чем сработает штатное восстановление шины ниже.
    Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
    Wire1.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
    scanBus("Wire (I2C0, GPIO21/18)", Wire);
    scanBus("Wire1 (I2C1, GPIO47/8)", Wire1);
    for (auto& ctx : g_sensors) {
        ctx.sht.begin(ctx.addr);
    }
}

// Восстанавливает всю общую шину один раз и заново инициализирует каждый
// датчик на ней (сброс шины затрагивает оба устройства, делящих SDA/SCL).
// Счётчики неудач на шине сбрасываются независимо от результата, чтобы
// постоянно отключённый датчик не устраивал шторм восстановлений на каждом
// цикле опроса — он просто продолжает показывать «ошибку» и переопрашивается
// в обычном темпе.
void recoverBusFor(SensorContext& failedCtx) {
    I2CRecovery::recoverBus(*failedCtx.bus, failedCtx.sdaPin, failedCtx.sclPin, I2C_CLOCK_HZ);
    for (auto& ctx : g_sensors) {
        if (ctx.bus == failedCtx.bus) {
            ctx.sht.begin(ctx.addr);
            ctx.consecutiveFailures = 0;
        }
    }
}

// Снимает датчик с обычного темпа опроса на SENSOR_OFFLINE_RETRY_MS —
// вызывается, когда он не отвечает даже сразу после recoverBusFor. Без
// этого физически не подключённый датчик продолжал бы конкурировать за
// общую шину на каждом цикле опроса, замедляя соседа на этой же шине.
void markOffline(SensorContext& ctx) {
    ctx.offline = true;
    ctx.nextRetryMs = millis() + SENSOR_OFFLINE_RETRY_MS;
}

void pollOneSensor(SensorContext& ctx) {
    float t = ctx.sht.readTemperature();
    float rh = ctx.sht.readHumidity();

    SensorReading reading;
    bool ok = !isnan(t) && !isnan(rh);

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
            float retryT = ctx.sht.readTemperature();
            float retryRh = ctx.sht.readHumidity();
            if (isnan(retryT) || isnan(retryRh)) {
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
                // опрос остальных датчиков на общей шине.
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

}  // namespace

namespace Sensors {

void begin() {
    xTaskCreatePinnedToCore(sensorTask, "sensorTask", 4096, nullptr, 3, nullptr, 0);
}

}  // namespace Sensors
