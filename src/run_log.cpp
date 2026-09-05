#include "run_log.h"

#include <algorithm>
#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <type_traits>

#include "config.h"
#include "settings_store.h"
#include "time_sync.h"

namespace {

constexpr const char* kLogPath = "/runlog.bin";
constexpr uint32_t kMagic = 0x484C4731;  // "HLG1"
constexpr uint32_t kCapacity = RUN_LOG_CAPACITY;
constexpr uint32_t kNoOpenSlot = 0xFFFFFFFFUL;

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic = kMagic;
    uint32_t capacity = kCapacity;
    uint32_t writeIndex = 0;            // куда будет записана следующая новая запись
    uint32_t count = 0;                  // сколько слотов реально заполнено (<= capacity)
    uint32_t openIndex = kNoOpenSlot;     // индекс незакрытой записи, если реле сейчас работает
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable<RunLog::RunRecord>::value, "RunRecord must be safe to read/write raw");
static_assert(std::is_trivially_copyable<FileHeader>::value, "FileHeader must be safe to read/write raw");

SemaphoreHandle_t g_mutex = nullptr;
FileHeader g_header;
bool g_ready = false;

bool lock() { return g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(300)) == pdTRUE; }

size_t slotOffset(uint32_t index) {
    return sizeof(FileHeader) + static_cast<size_t>(index) * sizeof(RunLog::RunRecord);
}

bool writeHeader() {
    File f = LittleFS.open(kLogPath, "r+");
    if (!f) return false;
    f.seek(0);
    bool ok = f.write(reinterpret_cast<const uint8_t*>(&g_header), sizeof(g_header)) == sizeof(g_header);
    f.close();
    return ok;
}

bool writeRecordAt(uint32_t index, const RunLog::RunRecord& rec) {
    File f = LittleFS.open(kLogPath, "r+");
    if (!f) return false;
    f.seek(slotOffset(index));
    bool ok = f.write(reinterpret_cast<const uint8_t*>(&rec), sizeof(rec)) == sizeof(rec);
    f.close();
    return ok;
}

bool readRecordAt(uint32_t index, RunLog::RunRecord& rec) {
    File f = LittleFS.open(kLogPath, "r");
    if (!f) return false;
    f.seek(slotOffset(index));
    bool ok = f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) == sizeof(rec);
    f.close();
    return ok;
}

// Читает index-ю от конца запись (0 = самая свежая) через уже открытый файл
// — используется в сканах (getSummary/getRecent), чтобы не открывать/
// закрывать файл на каждую запись отдельно.
bool readFromOpenFile(File& f, uint32_t slot, RunLog::RunRecord& rec) {
    f.seek(slotOffset(slot));
    return f.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) == sizeof(rec);
}

uint32_t slotFromNewest(uint32_t distanceFromNewest) {
    return (g_header.writeIndex + kCapacity - 1 - (distanceFromNewest % kCapacity)) % kCapacity;
}

// Создаёт файл заново: заголовок + capacity нулевых записей. Единственный
// относительно тяжёлый путь (RUN_LOG_CAPACITY записей по одной) — но
// выполняется только один раз за всё время жизни устройства (первая
// прошивка либо файл повреждён/несовместимого формата).
bool createFreshFile() {
    File f = LittleFS.open(kLogPath, "w");
    if (!f) return false;

    g_header = FileHeader{};
    bool ok = f.write(reinterpret_cast<const uint8_t*>(&g_header), sizeof(g_header)) == sizeof(g_header);

    RunLog::RunRecord zero{};
    for (uint32_t i = 0; ok && i < kCapacity; i++) {
        ok = f.write(reinterpret_cast<const uint8_t*>(&zero), sizeof(zero)) == sizeof(zero);
    }
    f.close();
    return ok;
}

}  // namespace

namespace RunLog {

const char* toString(StopReason reason) {
    switch (reason) {
        case StopReason::HysteresisReached:
            return "hysteresis_reached";
        case StopReason::ManualOff:
            return "manual_off";
        case StopReason::LockedFreeze:
            return "locked_freeze";
        case StopReason::LockedCondensation:
            return "locked_condensation";
        case StopReason::SensorFault:
            return "sensor_fault";
        case StopReason::Interrupted:
            return "interrupted";
        case StopReason::Unknown:
        default:
            return "unknown";
    }
}

void begin() {
    g_ready = false;
    if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
    if (!LittleFS.begin(false)) return;
    if (!lock()) return;

    bool needFresh = true;
    size_t expectedSize = sizeof(FileHeader) + sizeof(RunRecord) * kCapacity;
    File f = LittleFS.open(kLogPath, "r");
    if (f) {
        FileHeader hdr;
        if (f.size() == expectedSize && f.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)) == sizeof(hdr) &&
            hdr.magic == kMagic && hdr.capacity == kCapacity) {
            g_header = hdr;
            needFresh = false;
        }
        f.close();
    }

    if (needFresh && !createFreshFile()) {
        xSemaphoreGive(g_mutex);
        return;  // флеш недоступна/повреждена — журнал остаётся выключенным (g_ready=false)
    }

    // Реле всегда стартует выключенным (RelayController::begin()) — если
    // заголовок оставляет запись открытой, значит устройство перезагрузилось
    // посреди цикла. Закрываем её задним числом, а не оставляем висеть.
    if (g_header.openIndex != kNoOpenSlot) {
        RunRecord rec;
        bool recordPersisted = false;
        if (readRecordAt(g_header.openIndex, rec)) {
            if (rec.stopReason != StopReason::Unknown) {
                // Запись успела сохраниться до перезагрузки, а заголовок — нет.
                recordPersisted = true;
            } else {
                rec.endEpoch = TimeSync::nowEpoch();  // обычно всё ещё 0 — WiFi/NTP на этом этапе загрузки ещё не готовы
                rec.stopReason = StopReason::Interrupted;
                recordPersisted = writeRecordAt(g_header.openIndex, rec);
            }
        }
        if (recordPersisted) {
            g_header.openIndex = kNoOpenSlot;
            writeHeader();
        }
    }

    g_ready = true;
    xSemaphoreGive(g_mutex);
}

void recordStart(float crawlRh, float crawlTempC, float outsideRh, float outsideTempC) {
    if (!g_ready || !lock()) return;

    RunRecord rec{};
    rec.startEpoch = TimeSync::nowEpoch();
    rec.startCrawlRh = crawlRh;
    rec.startCrawlTempC = crawlTempC;
    rec.startOutsideRh = outsideRh;
    rec.startOutsideTempC = outsideTempC;

    uint32_t slot = g_header.writeIndex;
    if (writeRecordAt(slot, rec)) {
        g_header.openIndex = slot;
        g_header.writeIndex = (slot + 1) % kCapacity;
        if (g_header.count < kCapacity) g_header.count++;
        writeHeader();
    }

    xSemaphoreGive(g_mutex);
}

void recordStop(float crawlRh, float crawlTempC, float outsideRh, float outsideTempC, StopReason reason,
                uint32_t durationMs) {
    if (!g_ready || !lock()) return;
    if (g_header.openIndex == kNoOpenSlot) {
        xSemaphoreGive(g_mutex);
        return;  // не должно происходить (recordStart всегда предшествует recordStop) — защита от рассинхрона
    }

    RunRecord rec;
    bool recordPersisted = false;
    if (readRecordAt(g_header.openIndex, rec)) {
        rec.endEpoch = TimeSync::nowEpoch();
        rec.durationMs = durationMs;
        rec.endCrawlRh = crawlRh;
        rec.endCrawlTempC = crawlTempC;
        rec.endOutsideRh = outsideRh;
        rec.endOutsideTempC = outsideTempC;
        rec.stopReason = reason;
        recordPersisted = writeRecordAt(g_header.openIndex, rec);
    }

    if (recordPersisted) {
        g_header.openIndex = kNoOpenSlot;
        writeHeader();
    }
    xSemaphoreGive(g_mutex);
}

Summary getSummary() {
    Summary s;
    s.timeSynced = TimeSync::isSynced();
    s.runsTotal = Settings::loadCycleCount();

    if (!g_ready || !lock()) return s;
    if (!s.timeSynced || g_header.count == 0) {
        xSemaphoreGive(g_mutex);
        return s;
    }

    const uint32_t nowT = TimeSync::nowEpoch();
    const int64_t localNow = static_cast<int64_t>(nowT) + LOCAL_TZ_OFFSET_SEC;
    const uint32_t todayStart = static_cast<uint32_t>(localNow - (localNow % 86400) - LOCAL_TZ_OFFSET_SEC);
    const uint32_t todayEnd = todayStart + 86400;

    File f = LittleFS.open(kLogPath, "r");
    if (f) {
        RunRecord rec;
        for (uint32_t i = 0; i < g_header.count; i++) {
            if (!readFromOpenFile(f, slotFromNewest(i), rec)) break;
            if (rec.startEpoch == 0) continue;

            if (rec.startEpoch >= todayStart && rec.startEpoch < todayEnd) s.runsToday++;

            const uint32_t endEpoch = rec.stopReason == StopReason::Unknown ? nowT : rec.endEpoch;
            if (endEpoch == 0) continue;
            if (endEpoch <= todayStart) break;  // более старые циклы тоже не пересекают сегодняшний интервал

            const uint32_t overlapStart = std::max(rec.startEpoch, todayStart);
            const uint32_t overlapEnd = std::min(endEpoch, todayEnd);
            if (overlapEnd > overlapStart) s.runtimeTodayMs += (overlapEnd - overlapStart) * 1000;
        }
        f.close();
    }

    xSemaphoreGive(g_mutex);
    return s;
}

size_t getRecent(RunRecord* out, size_t maxCount, size_t offset) {
    if (out == nullptr || maxCount == 0 || !g_ready || !lock()) return 0;

    size_t available = g_header.count;
    size_t n = 0;
    File f = LittleFS.open(kLogPath, "r");
    if (f) {
        for (size_t i = offset; i < available && n < maxCount; i++) {
            if (readFromOpenFile(f, slotFromNewest(static_cast<uint32_t>(i)), out[n])) n++;
        }
        f.close();
    }

    xSemaphoreGive(g_mutex);
    return n;
}

size_t count() {
    if (!g_ready || !lock()) return 0;
    size_t c = g_header.count;
    xSemaphoreGive(g_mutex);
    return c;
}

size_t capacity() { return kCapacity; }

}  // namespace RunLog
