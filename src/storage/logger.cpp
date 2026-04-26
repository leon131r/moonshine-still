/**
 * @file logger.cpp
 * @brief Реализация NDJSON-логирования.
 *
 * Кольцевой буфер:
 * - Каждая запись — отдельная строка в PSRAM (до 256 байт)
 * - При заполнении — старые записи перезаписываются
 * - Сброс на диск: батчинг, все записи одной операцией
 *
 * Ротация файлов:
 * - Формат имени: log_001.ndjson, log_002.ndjson, ...
 * - При достижении rotation_mb — создаётся новый файл
 * - При превышении max_files — удаляется самый старый
 *
 * Формат NDJSON:
 * {"ts":123456,"phase":"body","T_top":78.20,"T_select":78.50,
 *  "T_below":78.90,"T_cooler":25.00,"pid_out":45.20,
 *  "heater_type":"electric","errors":0}
 */

#include "logger.h"
#include "storage/fs_manager.h"
#include <ArduinoJson.h>

// Максимальная длина одной NDJSON-строки
static constexpr size_t MAX_ENTRY_LEN = 256;
// Интервал сброса на диск (мс)
static constexpr uint32_t FLUSH_INTERVAL_MS = 5000;

// ============================================================================
// Singleton
// ============================================================================

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool Logger::begin(SettingsManager& settings) {
    settings_ = &settings;

    if (!allocateBuffer()) {
        Serial.println("[LOGGER] ERROR: Failed to allocate ring buffer");
        return false;
    }

    const SystemConfig& cfg = settings.getConfig();

    // Создаём директорию для логов если её нет
    bool dir_ok = false;
    
    if (FSManager::getInstance().exists(cfg.log_path)) {
        // Проверяем — это файл или директория?
        File d = FSManager::getInstance().openDir(cfg.log_path);
        if (d) {
            dir_ok = true;
            Serial.printf("[LOGGER] Log dir exists: %s\n", cfg.log_path);
            d.close();
        } else {
            // Это файл — удаляем
            Serial.printf("[LOGGER] WARNING: %s is a file, removing\n", cfg.log_path);
            FSManager::getInstance().removeFile(cfg.log_path);
        }
    }
    
    if (!dir_ok) {
        Serial.printf("[LOGGER] Creating dir: %s\n", cfg.log_path);
        if (FSManager::getInstance().mkdir(cfg.log_path)) {
            Serial.println("[LOGGER] mkdir OK");
            dir_ok = true;
        } else {
            Serial.println("[LOGGER] WARNING: mkdir failed, using root /");
        }
    }

    // Открываем первый файл
    if (!openNewFile()) {
        Serial.println("[LOGGER] ERROR: Failed to create log file");
        freeBuffer();
        return false;
    }

    flush_timer_ = millis();
    initialized_ = true;

    Serial.printf("[LOGGER] Initialized: path=%s, buf_size=%d\n",
                  cfg.log_path, ring_buf_.size);
    return true;
}

// ============================================================================
// log
// ============================================================================

void Logger::log(const SystemSnapshot& snapshot) {
    if (!initialized_) return;

    // Сериализуем в строку
    char entry[MAX_ENTRY_LEN];
    size_t len = snapshotToJSON(snapshot, entry, sizeof(entry));

    if (len == 0 || len >= MAX_ENTRY_LEN) {
        return; // Ошибка сериализации
    }

    // Добавляем в кольцевой буфер
    uint16_t idx = ring_buf_.head;

    // Если есть старая строка — освобождаем
    if (ring_buf_.entries[idx] != nullptr) {
        free(ring_buf_.entries[idx]);
    }

    // Копируем новую строку
    ring_buf_.entries[idx] = (char*)malloc(len + 1);
    if (ring_buf_.entries[idx] == nullptr) {
        return; // Нехватка памяти
    }
    memcpy(ring_buf_.entries[idx], entry, len + 1);

    // Двигаем head
    ring_buf_.head = (idx + 1) % ring_buf_.size;

    if (ring_buf_.count < ring_buf_.size) {
        ring_buf_.count++;
    } else {
        ring_buf_.full = true;
    }

    total_logged_++;
}

// ============================================================================
// logRaw
// ============================================================================

void Logger::logRaw(const char* json_str) {
    if (!initialized_ || !json_str) return;

    size_t len = strlen(json_str);
    if (len == 0 || len >= MAX_ENTRY_LEN) return;

    uint16_t idx = ring_buf_.head;

    if (ring_buf_.entries[idx] != nullptr) {
        free(ring_buf_.entries[idx]);
    }

    ring_buf_.entries[idx] = (char*)malloc(len + 1);
    if (ring_buf_.entries[idx] == nullptr) return;

    memcpy(ring_buf_.entries[idx], json_str, len + 1);
    ring_buf_.head = (idx + 1) % ring_buf_.size;

    if (ring_buf_.count < ring_buf_.size) {
        ring_buf_.count++;
    } else {
        ring_buf_.full = true;
    }

    total_logged_++;
}

// ============================================================================
// flush
// ============================================================================

bool Logger::flush() {
    if (!initialized_ || ring_buf_.count == 0) {
        return true; // Нечего сбрасывать
    }

    // Проверяем ротацию
    checkRotation();

    // Собираем все записи в один буфер
    // Находим «начало» (самая старая запись)
    uint16_t start = ring_buf_.full ? ring_buf_.head : 0;

    // Открываем файл для дозаписи через FSManager
    File f = FSManager::getInstance().openFile(current_file_, "a");
    if (!f) {
        Serial.println("[LOGGER] ERROR: Cannot open file for flush");
        return false;
    }

    size_t total_written = 0;
    for (uint16_t i = 0; i < ring_buf_.count; i++) {
        uint16_t idx = (start + i) % ring_buf_.size;
        if (ring_buf_.entries[idx] != nullptr) {
            size_t len = strlen(ring_buf_.entries[idx]);
            size_t written = f.write((const uint8_t*)ring_buf_.entries[idx], len);
            // Добавляем newline
            f.write((const uint8_t*)"\n", 1);
            total_written += written + 1;
        }
    }

    f.close();

    // Очищаем буфер после сброса
    for (uint16_t i = 0; i < ring_buf_.size; i++) {
        if (ring_buf_.entries[i] != nullptr) {
            free(ring_buf_.entries[i]);
            ring_buf_.entries[i] = nullptr;
        }
    }
    ring_buf_.head = 0;
    ring_buf_.count = 0;
    ring_buf_.full = false;

    Serial.printf("[LOGGER] Flushed %d entries (%d bytes) to %s\n",
                  ring_buf_.count, total_written, current_file_);
    return true;
}

// ============================================================================
// poll
// ============================================================================

void Logger::poll() {
    if (!initialized_) return;

    uint32_t now = millis();

    // Проверяем таймер сброса
    if (now - flush_timer_ >= FLUSH_INTERVAL_MS) {
        flush_timer_ = now;

        // Сбрасываем только если буфер не пуст
        if (ring_buf_.count > 0) {
            flush();
        }
    }
}

// ============================================================================
// getLastEntries
// ============================================================================

uint16_t Logger::getLastEntries(char* out_buf, size_t buf_size, uint16_t max_entries) {
    if (!initialized_ || !out_buf || buf_size == 0) {
        return 0;
    }

    uint16_t count = (max_entries < ring_buf_.count) ? max_entries : ring_buf_.count;
    if (count == 0) return 0;

    // Начинаем с самых старых записей
    uint16_t start = ring_buf_.full ? ring_buf_.head : 0;
    size_t pos = 0;

    for (uint16_t i = 0; i < count; i++) {
        uint16_t idx = (start + i) % ring_buf_.size;
        if (ring_buf_.entries[idx] == nullptr) continue;

        size_t entry_len = strlen(ring_buf_.entries[idx]);

        // Проверяем, влезет ли запись (+ newline + null)
        if (pos + entry_len + 2 > buf_size) {
            break; // Буфер заполнен
        }

        memcpy(out_buf + pos, ring_buf_.entries[idx], entry_len);
        pos += entry_len;
        out_buf[pos++] = '\n';
        out_buf[pos] = '\0';
    }

    return count;
}

// ============================================================================
// getBufferedCount
// ============================================================================

uint16_t Logger::getBufferedCount() const {
    return ring_buf_.count;
}

// ============================================================================
// allocateBuffer
// ============================================================================

bool Logger::allocateBuffer() {
    ring_buf_.size = LOG_BUFFER_SIZE;
    ring_buf_.head = 0;
    ring_buf_.count = 0;
    ring_buf_.full = false;

    // Выделяем массив указателей
    ring_buf_.entries = (char**)malloc(ring_buf_.size * sizeof(char*));
    if (ring_buf_.entries == nullptr) {
        return false;
    }

    // Инициализируем nullptr-ами
    memset(ring_buf_.entries, 0, ring_buf_.size * sizeof(char*));

    Serial.printf("[LOGGER] Ring buffer allocated: %d entries\n", ring_buf_.size);
    return true;
}

// ============================================================================
// freeBuffer
// ============================================================================

void Logger::freeBuffer() {
    if (ring_buf_.entries != nullptr) {
        for (uint16_t i = 0; i < ring_buf_.size; i++) {
            if (ring_buf_.entries[i] != nullptr) {
                free(ring_buf_.entries[i]);
            }
        }
        free(ring_buf_.entries);
        ring_buf_.entries = nullptr;
    }
}

// ============================================================================
// checkRotation
// ============================================================================

bool Logger::checkRotation() {
    // Проверяем размер текущего файла через FSManager
    size_t file_size = FSManager::getInstance().fileSize(current_file_);

    // Конвертируем MB в байты
    const SystemConfig& cfg = settings_->getConfig();
    size_t max_size = (size_t)cfg.log_rotation_mb * 1024 * 1024;

    if (file_size >= max_size) {
        Serial.printf("[LOGGER] Rotation: %s reached %d bytes (limit: %d MB)\n",
                      current_file_, file_size, cfg.log_rotation_mb);
        return openNewFile();
    }

    return false;
}

// ============================================================================
// openNewFile
// ============================================================================

bool Logger::openNewFile() {
    const SystemConfig& cfg = settings_->getConfig();
    
    // Определяем директорию: если /log существует как директория — используем её, иначе /
    const char* log_dir = "/";
    File d = FSManager::getInstance().openDir(cfg.log_path);
    if (d) {
        log_dir = cfg.log_path;
        d.close();
    }

    // Считаем существующие лог-файлы в директории
    uint8_t file_count = 0;
    File root = FSManager::getInstance().openDir(log_dir);
    if (root) {
        File f = root.openNextFile();
        while (f) {
            const char* name = f.name();
            // Считаем только log_*.ndjson
            if (strstr(name, "log_") == name && strstr(name, ".ndjson")) {
                file_count++;
            }
            f.close();
            f = root.openNextFile();
        }
        root.close();
    }

    // Если превысили max_files — удаляем самый старый
    if (file_count >= cfg.log_max_files) {
        char del_name[64];
        snprintf(del_name, sizeof(del_name), "%s/log_001.ndjson", log_dir);
        FSManager::getInstance().removeFile(del_name);
        Serial.printf("[LOGGER] Deleted old log: %s\n", del_name);
    }

    // Генерируем имя: log_path/log_XXX.ndjson
    uint16_t seq = file_count + 1;
    snprintf(current_file_, sizeof(current_file_), "%s/log_%03d.ndjson", log_dir, seq);

    Serial.printf("[LOGGER] New log file: %s (count: %d)\n", current_file_, file_count);
    return true;
}

// ============================================================================
// snapshotToJSON
// ============================================================================

size_t Logger::snapshotToJSON(const SystemSnapshot& snapshot, char* buf, size_t buf_size) {
    // Используем небольшой JsonDocument на стеке
    JsonDocument doc;

    // Базовые поля
    doc["ts"] = (uint32_t)(millis() / 1000); // UNIX-подобная метка (секунды от старта)
    doc["uptime"] = snapshot.uptime_sec;
    doc["phase"] = phaseToString(snapshot.phase);
    doc["delta_t"] = snapshot.delta_t;
    doc["heater_power"] = snapshot.heater_power;
    doc["pid_out_cube"] = snapshot.pid_out_cube;
    doc["pid_out_cooler"] = snapshot.pid_out_cooler;
    doc["heater_type"] = heaterTypeToString(snapshot.heater_type);
    doc["errors"] = snapshot.error_count;
    doc["calibrating"] = snapshot.calibrating;

    // Температуры датчиков
    for (uint8_t i = 0; i < snapshot.sensor_count; i++) {
        const SensorData& s = snapshot.sensors[i];
        if (s.present) {
            char key_raw[32];
            char key_corr[32];
            snprintf(key_raw, sizeof(key_raw), "T_%s_raw", roleToString(s.role));
            snprintf(key_corr, sizeof(key_corr), "T_%s", roleToString(s.role));
            doc[key_raw] = s.temp_raw;
            doc[key_corr] = s.temp_corrected;
        }
    }

    return serializeJson(doc, buf, buf_size);
}
