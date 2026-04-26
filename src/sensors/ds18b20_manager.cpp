/**
 * @file ds18b20_manager.cpp
 * @brief Реализация менеджера DS18B20.
 *
 * Неблокирующий опрос — ключевой принцип.
 * Конвертация 12-bit занимает 750 мс, поэтому:
 * - REQUEST: отправляем команду
 * - WAITING: ждём 800 мс (с запасом)
 * - READING: читаем значения
 * - IDLE: пауза до следующего цикла (SENSOR_POLL_MS)
 *
 * Калибровка:
 * - Собирает CALIBRATION_SAMPLES отсчётов с интервалом CALIBRATION_INTERVAL
 * - Вычисляет среднее для каждого датчика
 * - Результат передаётся в CalibrationManager
 */

#include "sensors/ds18b20_manager.h"
#include "sensors/calibration.h"
#include <Arduino.h>

// Константа DallasTemperature для невалидного значения
#define DEVICE_DISCONNECTED -127.0f

// ============================================================================
// Singleton
// ============================================================================

DS18B20Manager& DS18B20Manager::getInstance() {
    static DS18B20Manager instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool DS18B20Manager::begin(uint8_t one_wire_pin, SettingsManager& settings) {
    settings_ = &settings;

    // Инициализация OneWire
    one_wire_ = new OneWire(one_wire_pin);
    if (!one_wire_) {
        Serial.println("[DS18B20] ERROR: Failed to create OneWire object");
        return false;
    }

    // Инициализация DallasTemperature
    dallas_ = new DallasTemperature(one_wire_);
    if (!dallas_) {
        Serial.println("[DS18B20] ERROR: Failed to create DallasTemperature object");
        delete one_wire_;
        one_wire_ = nullptr;
        return false;
    }

    dallas_->begin();

    // Сканирование шины
    detected_count_ = scanBus();
    Serial.printf("[DS18B20] Found %d sensors on bus\n", detected_count_);

    if (detected_count_ == 0) {
        Serial.println("[DS18B20] WARNING: No DS18B20 sensors found!");
        return false;
    }

    // Сопоставление с конфигом
    matchWithConfig();

    // Установка разрешения 12-bit для всех обнаруженных датчиков
    for (uint8_t i = 0; i < detected_count_; i++) {
        dallas_->setResolution(detected_addrs_[i], 12);
    }

    dallas_->setWaitForConversion(false); // Асинхронный режим!

    // Инициализация состояния опроса
    poll_state_ = PollState::IDLE;
    poll_timer_ = millis();

    Serial.println("[DS18B20] Initialized successfully");
    return true;
}

// ============================================================================
// poll (неблокирующий опрос)
// ============================================================================

void DS18B20Manager::poll() {
    if (!dallas_ || detected_count_ == 0) {
        return;
    }

    // Если идёт калибровка — обрабатываем отдельно
    if (calibrating_) {
        pollCalibration();
        return;
    }

    uint32_t now = millis();

    switch (poll_state_) {
        case PollState::IDLE:
            // Ждём SENSOR_POLL_MS с момента последнего чтения
            if (now - poll_timer_ >= SENSOR_POLL_MS) {
                poll_state_ = PollState::REQUEST;
            }
            break;

        case PollState::REQUEST:
            // Отправляем команду конвертации (асинхронно!)
            dallas_->requestTemperatures();
            poll_timer_ = now;
            poll_state_ = PollState::WAITING;
            break;

        case PollState::WAITING:
            // Ждём завершения конвертации (800 мс для 12-bit с запасом)
            if (now - poll_timer_ >= 800) {
                poll_state_ = PollState::READING;
            }
            break;

        case PollState::READING:
            // Читаем температуры всех датчиков
            for (uint8_t i = 0; i < detected_count_; i++) {
                float raw = dallas_->getTempC(detected_addrs_[i]);

                // Проверяем валидность
                if (raw == DEVICE_DISCONNECTED) {
                    sensors_[i].present = false;
                    sensors_[i].temp_raw = 0;
                    sensors_[i].temp_corrected = 0;
                    Serial.printf("[DS18B20] WARNING: Sensor %d disconnected\n", i);
                } else {
                    sensors_[i].present = true;
                    sensors_[i].temp_raw = raw;
                    sensors_[i].temp_corrected = applyCorrection(i, raw);
                }
                sensors_[i].last_update_ms = now;
            }

            poll_timer_ = now;
            poll_state_ = PollState::IDLE;
            break;
    }
}

// ============================================================================
// scanBus
// ============================================================================

uint8_t DS18B20Manager::scanBus() {
    uint8_t count = 0;
    uint8_t addr[8];

    one_wire_->reset_search();

    while (one_wire_->search(addr) && count < MAX_SENSORS) {
        // Проверяем CRC (последний байт — CRC)
        if (OneWire::crc8(addr, 7) != addr[7]) {
            Serial.println("[DS18B20] WARNING: CRC mismatch, skipping device");
            continue;
        }

        // Проверяем тип устройства (family code 0x28 = DS18B20)
        if (addr[0] != 0x28) {
            continue; // Не DS18B20
        }

        // Сохраняем адрес
        memcpy(detected_addrs_[count], addr, 8);
        addressToHex(addr, sensors_[count].address_hex, sizeof(sensors_[count].address_hex));

        Serial.printf("[DS18B20] Device %d: %s\n", count, sensors_[count].address_hex);
        count++;
    }

    return count;
}

// ============================================================================
// matchWithConfig
// ============================================================================

void DS18B20Manager::matchWithConfig() {
    const SystemConfig& cfg = settings_->getConfig();

    // Для каждого датчика из конфига ищем совпадение на шине
    for (uint8_t c = 0; c < cfg.sensor_count; c++) {
        const SensorData& cfg_sensor = cfg.sensors[c];

        // Ищем совпадение адреса среди обнаруженных
        for (uint8_t d = 0; d < detected_count_; d++) {
            if (strcmp(cfg_sensor.address_hex, sensors_[d].address_hex) == 0) {
                // Совпадение найдено — копируем метаданные из конфига
                sensors_[d].role = cfg_sensor.role;
                strncpy(sensors_[d].name, cfg_sensor.name, MAX_NAME_LEN - 1);
                sensors_[d].calibrate = cfg_sensor.calibrate;
                sensors_[d].offset = cfg_sensor.offset;
                sensors_[d].manual_offset = cfg_sensor.manual_offset;
                sensors_[d].present = false; // Будет обновлено при первом опросе
                break;
            }
        }
    }

    // Логируем соответствие
    for (uint8_t i = 0; i < detected_count_; i++) {
        Serial.printf("[DS18B20] Sensor %d: %s role=%s name=%s cal=%d\n",
                      i,
                      sensors_[i].address_hex,
                      roleToString(sensors_[i].role),
                      sensors_[i].name,
                      sensors_[i].calibrate);
    }
}

// ============================================================================
// applyCorrection
// ============================================================================

float DS18B20Manager::applyCorrection(uint8_t index, float raw_temp) const {
    if (index >= detected_count_) {
        return raw_temp;
    }

    if (sensors_[index].calibrate) {
        // Автоматическая калибровка: T_corrected = T_raw + offset
        return raw_temp + sensors_[index].offset;
    } else {
        // Ручное смещение
        return raw_temp + sensors_[index].manual_offset;
    }
}

// ============================================================================
// addressToHex
// ============================================================================

void DS18B20Manager::addressToHex(const uint8_t* addr, char* hex_buf, size_t buf_size) {
    size_t max_chars = (buf_size < 17) ? buf_size : 17;
    for (size_t i = 0; i < 8 && (i * 2 + 2) < max_chars; i++) {
        snprintf(hex_buf + i * 2, 3, "%02X", addr[i]);
    }
    hex_buf[16] = '\0';
}

// ============================================================================
// getSensorByRole
// ============================================================================

const SensorData* DS18B20Manager::getSensorByRole(SensorRole role) const {
    for (uint8_t i = 0; i < detected_count_; i++) {
        if (sensors_[i].role == role) {
            return &sensors_[i];
        }
    }
    return nullptr;
}

// ============================================================================
// getTemp
// ============================================================================

float DS18B20Manager::getTemp(uint8_t index) const {
    if (index >= detected_count_) {
        return DEVICE_DISCONNECTED;
    }
    return sensors_[index].temp_corrected;
}

// ============================================================================
// allSensorsValid
// ============================================================================

bool DS18B20Manager::allSensorsValid() const {
    if (detected_count_ == 0) return false;

    for (uint8_t i = 0; i < detected_count_; i++) {
        if (!sensors_[i].present) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// startCalibration
// ============================================================================

bool DS18B20Manager::startCalibration() {
    if (calibrating_) {
        return false; // Уже идёт
    }

    // Находим датчики с calibrate = true
    bool has_calib_sensors = false;
    for (uint8_t i = 0; i < detected_count_; i++) {
        if (sensors_[i].calibrate) {
            has_calib_sensors = true;
            break;
        }
    }

    if (!has_calib_sensors) {
        Serial.println("[DS18B20] WARNING: No sensors with calibrate=true");
        return false;
    }

    calibrating_ = true;
    calib_samples_ = 0;
    memset(calib_sum_, 0, sizeof(calib_sum_));
    calib_timer_ = millis();

    Serial.println("[DS18B20] Calibration started");
    return true;
}

// ============================================================================
// pollCalibration
// ============================================================================

void DS18B20Manager::pollCalibration() {
    uint32_t now = millis();

    if (calib_samples_ >= CALIBRATION_SAMPLES) {
        // Калибровка завершена — вычисляем средние и передаём в CalibrationManager
        float averages[MAX_SENSORS];

        for (uint8_t i = 0; i < detected_count_; i++) {
            if (sensors_[i].calibrate && calib_sum_[i] > 0) {
                averages[i] = calib_sum_[i] / CALIBRATION_SAMPLES;
            } else {
                averages[i] = 0;
            }
        }

        // Вычисляем смещения
        CalibrationManager& cal = CalibrationManager::getInstance();
        cal.computeOffsets(sensors_, averages, detected_count_);

        // Сохраняем в настройки
        float offsets[MAX_SENSORS];
        uint8_t offset_count = 0;
        for (uint8_t i = 0; i < detected_count_; i++) {
            if (sensors_[i].calibrate) {
                offsets[offset_count++] = sensors_[i].offset;
            }
        }

        settings_->saveCalibrationOffsets(offsets, offset_count);

        calibrating_ = false;
        Serial.println("[DS18B20] Calibration completed and saved");
        return;
    }

    // Ждём интервал между отсчётами
    if (now - calib_timer_ < CALIBRATION_INTERVAL) {
        return;
    }
    calib_timer_ = now;

    // Запрашиваем температуры
    dallas_->requestTemperatures();
    delay(800); // При калибровке допустима блокировка (запускается редко)

    // Суммируем
    for (uint8_t i = 0; i < detected_count_; i++) {
        float t = dallas_->getTempC(detected_addrs_[i]);
        if (t != DEVICE_DISCONNECTED && sensors_[i].calibrate) {
            calib_sum_[i] += t;
        }
    }

    calib_samples_++;
}

// ============================================================================
// getCalibrationProgress
// ============================================================================

uint8_t DS18B20Manager::getCalibrationProgress() const {
    if (!calibrating_) {
        return 100;
    }
    return (uint8_t)((calib_samples_ * 100) / CALIBRATION_SAMPLES);
}
