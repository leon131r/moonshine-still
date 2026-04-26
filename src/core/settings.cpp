/**
 * @file settings.cpp
 * @brief Реализация менеджера настроек.
 *
 * Использует ArduinoJson для парсинга/сериализации.
 * При наличии PSRAM — выделяет буфер через ps_malloc(),
 * иначе — стандартный heap.
 *
 * Формат settings.json строго соответствует структуре SystemConfig.
 * Missing-поля заполняются дефолтами (backward compatibility).
 *
 * Новые поля (добавлены v1.1):
 * - system.mode — distillation / rectification
 * - rectification.sub_mode — vapor / liquid
 * - heater.cooler_type — fan / servo
 * - heater.gas_control_mode — auto / remote_pid / manual
 * - distillation.target_temp, temp_tolerance
 * - pid_dist_cooler — PID охлаждения для дистилляции
 * - sensors[].active — участие датчика в расчётах
 */

#include "settings.h"
#include "storage/fs_manager.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cmath>

// Размер JSON-документа. ~3KB чтение, ~4KB запись (с отступами, новые поля)
static constexpr size_t JSON_DOC_SIZE = 6144;

// Интервал проверки файла (мс)
static constexpr uint32_t WATCH_INTERVAL_MS = 5000;

// ============================================================================
// Singleton
// ============================================================================

SettingsManager& SettingsManager::getInstance() {
    static SettingsManager instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool SettingsManager::begin(const char* fs_path) {
    strncpy(file_path_, fs_path, sizeof(file_path_) - 1);
    file_path_[sizeof(file_path_) - 1] = '\0';

    // Проверяем существование файла
    if (!LittleFS.exists(file_path_)) {
        Serial.println("[SETTINGS] File not found, applying defaults");
        applyDefaults();
        save();
        return false;
    }

    // Загружаем из файла
    if (!load()) {
        Serial.println("[SETTINGS] Parse error, applying defaults");
        applyDefaults();
        save();
        return false;
    }

    Serial.println("[SETTINGS] Loaded successfully");
    return true;
}

// ============================================================================
// load
// ============================================================================

bool SettingsManager::load() {
    // Выделяем буфер: приоритет PSRAM
    uint8_t* buf = nullptr;
#if CONFIG_SPIRAM_SUPPORT
    buf = (uint8_t*)ps_malloc(JSON_DOC_SIZE);
    if (buf == nullptr) {
#endif
    buf = (uint8_t*)malloc(JSON_DOC_SIZE);
#if CONFIG_SPIRAM_SUPPORT
    }
#endif

    if (buf == nullptr) {
        Serial.println("[SETTINGS] ERROR: No memory for JSON buffer");
        return false;
    }

    // Читаем файл
    File f = LittleFS.open(file_path_, "r");
    if (!f) {
        Serial.println("[SETTINGS] ERROR: Cannot open file");
        free(buf);
        return false;
    }

    size_t len = f.readBytes((char*)buf, JSON_DOC_SIZE - 1);
    buf[len] = '\0';
    f.close();

    // Парсим JSON
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, (const char*)buf);
    free(buf);

    if (err) {
        Serial.printf("[SETTINGS] JSON error: %s\n", err.c_str());
        return false;
    }

    // ========================================================================
    // system
    // ========================================================================
    if (doc["system"].is<JsonObject>()) {
        if (doc["system"]["device_name"].is<const char*>())
            strncpy(config_.device_name, doc["system"]["device_name"], MAX_DEV_NAME_LEN - 1);
        if (doc["system"]["wifi_ssid"].is<const char*>())
            strncpy(config_.wifi_ssid, doc["system"]["wifi_ssid"], CFG_MAX_SSID_LEN - 1);
        if (doc["system"]["wifi_pass"].is<const char*>())
            strncpy(config_.wifi_pass, doc["system"]["wifi_pass"], CFG_MAX_WIFI_PASS_LEN - 1);
        // Режим работы (новое поле v1.1)
        if (doc["system"]["mode"].is<const char*>())
            config_.mode = stringToMode(doc["system"]["mode"]);
    }

    // ========================================================================
    // sensors — ОБНУЛЯЕМ перед парсингом (баг BUG-NEW-009)
    // ========================================================================
    config_.sensor_count = 0;
    memset(config_.sensors, 0, sizeof(config_.sensors));

    if (doc["sensors"].is<JsonArray>()) {
        JsonArray arr = doc["sensors"].as<JsonArray>();
        uint8_t idx = 0;
        for (JsonObject obj : arr) {
            if (idx >= MAX_SENSORS) break;

            SensorData& s = config_.sensors[idx];

            if (obj["address"].is<const char*>())
                strncpy(s.address_hex, obj["address"], sizeof(s.address_hex) - 1);
            if (obj["role"].is<const char*>())
                s.role = stringToRole(obj["role"]);
            if (obj["name"].is<const char*>())
                strncpy(s.name, obj["name"], MAX_NAME_LEN - 1);
            if (obj["calibrate"].is<bool>())
                s.calibrate = obj["calibrate"];
            if (obj["offset"].is<float>())
                s.offset = obj["offset"];
            if (obj["manual_offset"].is<float>())
                s.manual_offset = obj["manual_offset"];
            // Поле active (новое v1.1), по умолчанию true
            if (obj["active"].is<bool>())
                s.active = obj["active"];
            else
                s.active = true;

            // Runtime-поля (не из JSON)
            s.temp_raw = 0;
            s.temp_corrected = 0;
            s.present = false;
            s.last_update_ms = 0;

            idx++;
        }
        config_.sensor_count = idx;
    }

    Serial.printf("[SETTINGS] Sensors loaded: %d\n", config_.sensor_count);

    // ========================================================================
    // heater
    // ========================================================================
    if (doc["heater"].is<JsonObject>()) {
        if (doc["heater"]["type"].is<const char*>()) {
            const char* t = doc["heater"]["type"];
            config_.heater_type = (strcmp(t, "GAS") == 0) ? HeaterType::GAS : HeaterType::ELECTRIC;
        }
        if (doc["heater"]["pin_pwm"].is<uint8_t>())
            config_.pin_pwm = doc["heater"]["pin_pwm"];
        if (doc["heater"]["pin_cooler"].is<uint8_t>())
            config_.pin_cooler = doc["heater"]["pin_cooler"];
        if (doc["heater"]["espnow_mac_gas"].is<const char*>()) {
            const char* mac_str = doc["heater"]["espnow_mac_gas"];
            unsigned int m[6];
            if (sscanf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
                       &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                for (int i = 0; i < 6; i++) {
                    config_.espnow_mac_gas[i] = (uint8_t)m[i];
                }
            }
        }
        // cooler_type (новое v1.1)
        if (doc["heater"]["cooler_type"].is<const char*>())
            config_.cooler_type = stringToCoolerType(doc["heater"]["cooler_type"]);
        // gas_control_mode (новое v1.1)
        if (doc["heater"]["gas_control_mode"].is<const char*>())
            config_.gas_control_mode = stringToGasControlMode(doc["heater"]["gas_control_mode"]);
    }

    // ========================================================================
    // rectification (новое v1.1 — было phase_thresholds)
    // ========================================================================
    // Поддерживаем оба формата: новый "rectification" и старый "phase_thresholds"
    if (doc["rectification"].is<JsonObject>()) {
        if (doc["rectification"]["sub_mode"].is<const char*>())
            config_.rect_sub_mode = stringToSubMode(doc["rectification"]["sub_mode"]);
        if (doc["rectification"]["heads_end"].is<float>())
            config_.threshold_heads_end = doc["rectification"]["heads_end"];
        if (doc["rectification"]["body_end"].is<float>())
            config_.threshold_body_end = doc["rectification"]["body_end"];
        if (doc["rectification"]["delta_hysteresis"].is<float>())
            config_.delta_hysteresis = doc["rectification"]["delta_hysteresis"];
    } else if (doc["phase_thresholds"].is<JsonObject>()) {
        // Backward compatibility — старый формат
        if (doc["phase_thresholds"]["heads_end"].is<float>())
            config_.threshold_heads_end = doc["phase_thresholds"]["heads_end"];
        if (doc["phase_thresholds"]["body_end"].is<float>())
            config_.threshold_body_end = doc["phase_thresholds"]["body_end"];
        if (doc["phase_thresholds"]["delta_hysteresis"].is<float>())
            config_.delta_hysteresis = doc["phase_thresholds"]["delta_hysteresis"];
    }

    // ========================================================================
    // distillation (новое v1.1)
    // ========================================================================
    if (doc["distillation"].is<JsonObject>()) {
        if (doc["distillation"]["target_temp"].is<float>())
            config_.dist_target_temp = doc["distillation"]["target_temp"];
        if (doc["distillation"]["temp_tolerance"].is<float>())
            config_.dist_temp_tolerance = doc["distillation"]["temp_tolerance"];
    }

    // ========================================================================
    // pid_cube (PID куба / нагрева)
    // ========================================================================
    if (doc["pid_cube"].is<JsonObject>()) {
        if (doc["pid_cube"]["kp"].is<float>()) config_.pid_cube.kp = doc["pid_cube"]["kp"];
        if (doc["pid_cube"]["ki"].is<float>()) config_.pid_cube.ki = doc["pid_cube"]["ki"];
        if (doc["pid_cube"]["kd"].is<float>()) config_.pid_cube.kd = doc["pid_cube"]["kd"];
        if (doc["pid_cube"]["out_min"].is<float>()) config_.pid_cube.out_min = doc["pid_cube"]["out_min"];
        if (doc["pid_cube"]["out_max"].is<float>()) config_.pid_cube.out_max = doc["pid_cube"]["out_max"];
        if (doc["pid_cube"]["integral_max"].is<float>()) config_.pid_cube.integral_max = doc["pid_cube"]["integral_max"];
    }

    // ========================================================================
    // pid_cooler (PID охладителя — ректификация: дефлегматор)
    // ========================================================================
    if (doc["pid_cooler"].is<JsonObject>()) {
        if (doc["pid_cooler"]["kp"].is<float>()) config_.pid_cooler.kp = doc["pid_cooler"]["kp"];
        if (doc["pid_cooler"]["ki"].is<float>()) config_.pid_cooler.ki = doc["pid_cooler"]["ki"];
        if (doc["pid_cooler"]["kd"].is<float>()) config_.pid_cooler.kd = doc["pid_cooler"]["kd"];
        if (doc["pid_cooler"]["out_min"].is<float>()) config_.pid_cooler.out_min = doc["pid_cooler"]["out_min"];
        if (doc["pid_cooler"]["out_max"].is<float>()) config_.pid_cooler.out_max = doc["pid_cooler"]["out_max"];
        if (doc["pid_cooler"]["integral_max"].is<float>()) config_.pid_cooler.integral_max = doc["pid_cooler"]["integral_max"];
    }

    // ========================================================================
    // pid_dist_cooler (PID охладителя — дистилляция: конденсатор, новое v1.1)
    // ========================================================================
    if (doc["pid_dist_cooler"].is<JsonObject>()) {
        if (doc["pid_dist_cooler"]["kp"].is<float>()) config_.pid_dist_cooler.kp = doc["pid_dist_cooler"]["kp"];
        if (doc["pid_dist_cooler"]["ki"].is<float>()) config_.pid_dist_cooler.ki = doc["pid_dist_cooler"]["ki"];
        if (doc["pid_dist_cooler"]["kd"].is<float>()) config_.pid_dist_cooler.kd = doc["pid_dist_cooler"]["kd"];
        if (doc["pid_dist_cooler"]["out_min"].is<float>()) config_.pid_dist_cooler.out_min = doc["pid_dist_cooler"]["out_min"];
        if (doc["pid_dist_cooler"]["out_max"].is<float>()) config_.pid_dist_cooler.out_max = doc["pid_dist_cooler"]["out_max"];
        if (doc["pid_dist_cooler"]["integral_max"].is<float>()) config_.pid_dist_cooler.integral_max = doc["pid_dist_cooler"]["integral_max"];
    }

    // ========================================================================
    // mqtt
    // ========================================================================
    if (doc["mqtt"].is<JsonObject>()) {
        if (doc["mqtt"]["enabled"].is<bool>()) config_.mqtt_enabled = doc["mqtt"]["enabled"];
        if (doc["mqtt"]["host"].is<const char*>()) strncpy(config_.mqtt_host, doc["mqtt"]["host"], MAX_MQTT_HOST_LEN - 1);
        if (doc["mqtt"]["port"].is<uint16_t>()) config_.mqtt_port = doc["mqtt"]["port"];
        if (doc["mqtt"]["user"].is<const char*>()) strncpy(config_.mqtt_user, doc["mqtt"]["user"], MAX_CRED_LEN - 1);
        if (doc["mqtt"]["pass"].is<const char*>()) strncpy(config_.mqtt_pass, doc["mqtt"]["pass"], MAX_CRED_LEN - 1);
        if (doc["mqtt"]["topic_root"].is<const char*>()) strncpy(config_.mqtt_topic_root, doc["mqtt"]["topic_root"], MAX_TOPIC_LEN - 1);
    }

    // ========================================================================
    // logging
    // ========================================================================
    if (doc["logging"].is<JsonObject>()) {
        if (doc["logging"]["path"].is<const char*>()) strncpy(config_.log_path, doc["logging"]["path"], MAX_PATH_LEN - 1);
        if (doc["logging"]["max_files"].is<uint8_t>()) config_.log_max_files = doc["logging"]["max_files"];
        if (doc["logging"]["rotation_mb"].is<uint8_t>()) config_.log_rotation_mb = doc["logging"]["rotation_mb"];
    }

    // ========================================================================
    // tft
    // ========================================================================
    if (doc["tft"].is<JsonObject>()) {
        if (doc["tft"]["enabled"].is<bool>()) config_.tft_enabled = doc["tft"]["enabled"];
        if (doc["tft"]["controller"].is<const char*>())
            config_.tft_controller = stringToTFTController(doc["tft"]["controller"]);
        if (doc["tft"]["spi_clk"].is<uint8_t>()) config_.tft_spi_clk = doc["tft"]["spi_clk"];
        if (doc["tft"]["spi_mosi"].is<uint8_t>()) config_.tft_spi_mosi = doc["tft"]["spi_mosi"];
        if (doc["tft"]["spi_miso"].is<uint8_t>()) config_.tft_spi_miso = doc["tft"]["spi_miso"];
        if (doc["tft"]["spi_cs"].is<uint8_t>()) config_.tft_spi_cs = doc["tft"]["spi_cs"];
        if (doc["tft"]["spi_dc"].is<uint8_t>()) config_.tft_spi_dc = doc["tft"]["spi_dc"];
        if (doc["tft"]["spi_rst"].is<uint8_t>()) config_.tft_spi_rst = doc["tft"]["spi_rst"];
        if (doc["tft"]["backlight"].is<uint8_t>()) config_.tft_backlight = doc["tft"]["backlight"];
    }

    // Сохраняем mtime
    File f2 = LittleFS.open(file_path_, "r");
    if (f2) {
        file_mtime_ = f2.getLastWrite();
        f2.close();
    }

    // Валидация
    if (!validate()) {
        Serial.println("[SETTINGS] Validation failed, applying defaults");
        applyDefaults();
        return false;
    }

    return true;
}

// ============================================================================
// save
// ============================================================================

bool SettingsManager::save() {
    JsonDocument doc;

    // --- system ---
    doc["system"]["device_name"] = config_.device_name;
    doc["system"]["wifi_ssid"] = config_.wifi_ssid;
    doc["system"]["wifi_pass"] = config_.wifi_pass;
    doc["system"]["mode"] = modeToString(config_.mode);

    // --- sensors ---
    JsonArray sensors_arr = doc["sensors"].to<JsonArray>();
    for (uint8_t i = 0; i < config_.sensor_count; i++) {
        JsonObject obj = sensors_arr.add<JsonObject>();
        obj["address"] = config_.sensors[i].address_hex;
        obj["role"] = roleToString(config_.sensors[i].role);
        obj["name"] = config_.sensors[i].name;
        obj["calibrate"] = config_.sensors[i].calibrate;
        obj["offset"] = config_.sensors[i].offset;
        obj["manual_offset"] = config_.sensors[i].manual_offset;
        obj["active"] = config_.sensors[i].active;
    }

    // --- heater ---
    doc["heater"]["type"] = heaterTypeToString(config_.heater_type);
    doc["heater"]["pin_pwm"] = config_.pin_pwm;
    doc["heater"]["pin_cooler"] = config_.pin_cooler;

    // MAC в строковом формате
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             config_.espnow_mac_gas[0], config_.espnow_mac_gas[1],
             config_.espnow_mac_gas[2], config_.espnow_mac_gas[3],
             config_.espnow_mac_gas[4], config_.espnow_mac_gas[5]);
    doc["heater"]["espnow_mac_gas"] = mac_str;

    // Новые поля нагревателя (v1.1)
    doc["heater"]["cooler_type"] = coolerTypeToString(config_.cooler_type);
    doc["heater"]["gas_control_mode"] = gasControlModeToString(config_.gas_control_mode);

    // --- rectification ---
    doc["rectification"]["sub_mode"] = subModeToString(config_.rect_sub_mode);
    doc["rectification"]["heads_end"] = config_.threshold_heads_end;
    doc["rectification"]["body_end"] = config_.threshold_body_end;
    doc["rectification"]["delta_hysteresis"] = config_.delta_hysteresis;

    // --- distillation ---
    doc["distillation"]["target_temp"] = config_.dist_target_temp;
    doc["distillation"]["temp_tolerance"] = config_.dist_temp_tolerance;

    // --- pid_cube ---
    doc["pid_cube"]["kp"] = config_.pid_cube.kp;
    doc["pid_cube"]["ki"] = config_.pid_cube.ki;
    doc["pid_cube"]["kd"] = config_.pid_cube.kd;
    doc["pid_cube"]["out_min"] = config_.pid_cube.out_min;
    doc["pid_cube"]["out_max"] = config_.pid_cube.out_max;
    doc["pid_cube"]["integral_max"] = config_.pid_cube.integral_max;

    // --- pid_cooler ---
    doc["pid_cooler"]["kp"] = config_.pid_cooler.kp;
    doc["pid_cooler"]["ki"] = config_.pid_cooler.ki;
    doc["pid_cooler"]["kd"] = config_.pid_cooler.kd;
    doc["pid_cooler"]["out_min"] = config_.pid_cooler.out_min;
    doc["pid_cooler"]["out_max"] = config_.pid_cooler.out_max;
    doc["pid_cooler"]["integral_max"] = config_.pid_cooler.integral_max;

    // --- pid_dist_cooler (новое v1.1) ---
    doc["pid_dist_cooler"]["kp"] = config_.pid_dist_cooler.kp;
    doc["pid_dist_cooler"]["ki"] = config_.pid_dist_cooler.ki;
    doc["pid_dist_cooler"]["kd"] = config_.pid_dist_cooler.kd;
    doc["pid_dist_cooler"]["out_min"] = config_.pid_dist_cooler.out_min;
    doc["pid_dist_cooler"]["out_max"] = config_.pid_dist_cooler.out_max;
    doc["pid_dist_cooler"]["integral_max"] = config_.pid_dist_cooler.integral_max;

    // --- mqtt ---
    doc["mqtt"]["enabled"] = config_.mqtt_enabled;
    doc["mqtt"]["host"] = config_.mqtt_host;
    doc["mqtt"]["port"] = config_.mqtt_port;
    doc["mqtt"]["user"] = config_.mqtt_user;
    doc["mqtt"]["pass"] = config_.mqtt_pass;
    doc["mqtt"]["topic_root"] = config_.mqtt_topic_root;

    // --- logging ---
    doc["logging"]["path"] = config_.log_path;
    doc["logging"]["max_files"] = config_.log_max_files;
    doc["logging"]["rotation_mb"] = config_.log_rotation_mb;

    // --- tft ---
    doc["tft"]["enabled"] = config_.tft_enabled;
    doc["tft"]["controller"] = tftControllerToString(config_.tft_controller);
    doc["tft"]["spi_clk"] = config_.tft_spi_clk;
    doc["tft"]["spi_mosi"] = config_.tft_spi_mosi;
    doc["tft"]["spi_miso"] = config_.tft_spi_miso;
    doc["tft"]["spi_cs"] = config_.tft_spi_cs;
    doc["tft"]["spi_dc"] = config_.tft_spi_dc;
    doc["tft"]["spi_rst"] = config_.tft_spi_rst;
    doc["tft"]["backlight"] = config_.tft_backlight;

    // Сериализуем в файл
    File f = LittleFS.open(file_path_, "w");
    if (!f) {
        Serial.println("[SETTINGS] ERROR: Cannot write file");
        return false;
    }

    serializeJsonPretty(doc, f);
    f.close();

    // Обновляем mtime
    File f2 = LittleFS.open(file_path_, "r");
    if (f2) {
        file_mtime_ = f2.getLastWrite();
        f2.close();
    }

    Serial.println("[SETTINGS] Saved to file");
    return true;
}

// ============================================================================
// watch
// ============================================================================

void SettingsManager::watch() {
    static uint32_t last_check = 0;
    uint32_t now = millis();

    if (now - last_check < WATCH_INTERVAL_MS) {
        return;
    }
    last_check = now;

    // Проверяем mtime файла
    File f = LittleFS.open(file_path_, "r");
    if (!f) {
        return; // Файл недоступен — пропускаем
    }

    uint32_t current_mtime = f.getLastWrite();
    f.close();

    if (current_mtime != file_mtime_) {
        Serial.println("[SETTINGS] File changed externally, reloading");
        // Файл изменён — перечитываем
        if (load()) {
            changed_ = true;
        }
    }
}

// ============================================================================
// setGasMAC
// ============================================================================

void SettingsManager::setGasMAC(const uint8_t mac[6]) {
    memcpy(config_.espnow_mac_gas, mac, 6);
}

// ============================================================================
// saveCalibrationOffsets
// ============================================================================

bool SettingsManager::saveCalibrationOffsets(const float* offsets, uint8_t count) {
    if (count > config_.sensor_count) {
        count = config_.sensor_count;
    }
    for (uint8_t i = 0; i < count; i++) {
        config_.sensors[i].offset = offsets[i];
    }
    return save();
}

// ============================================================================
// savePhaseThresholds
// ============================================================================

bool SettingsManager::savePhaseThresholds(float heads_end, float body_end, float hysteresis) {
    config_.threshold_heads_end = heads_end;
    config_.threshold_body_end = body_end;
    config_.delta_hysteresis = hysteresis;
    return save();
}

// ============================================================================
// savePIDCubeParams
// ============================================================================

bool SettingsManager::savePIDCubeParams(const PIDParams& params) {
    config_.pid_cube = params;
    return save();
}

// ============================================================================
// savePIDCoolerParams
// ============================================================================

bool SettingsManager::savePIDCoolerParams(const PIDParams& params) {
    config_.pid_cooler = params;
    return save();
}

// ============================================================================
// savePIDDistCoolerParams (новое v1.1)
// ============================================================================

bool SettingsManager::savePIDDistCoolerParams(const PIDParams& params) {
    config_.pid_dist_cooler = params;
    return save();
}

// ============================================================================
// applyDefaults
// ============================================================================

void SettingsManager::applyDefaults() {
    // Обнуляем всю структуру
    memset(&config_, 0, sizeof(SystemConfig));

    // system
    strncpy(config_.device_name, "RectController_S3", MAX_DEV_NAME_LEN - 1);
    config_.wifi_ssid[0] = '\0';
    config_.wifi_pass[0] = '\0';
    config_.mode = OperationMode::RECTIFICATION;  // По умолчанию ректификация

    // sensors — обнулено memset
    config_.sensor_count = 0;

    // heater
    config_.heater_type = HeaterType::ELECTRIC;
    config_.pin_pwm = 17;
    config_.pin_cooler = 18;
    memset(config_.espnow_mac_gas, 0, 6);
    config_.cooler_type = CoolerType::FAN;          // По умолчанию вентилятор
    config_.gas_control_mode = GasControlMode::AUTO; // По умолчанию авто

    // rectification
    config_.rect_sub_mode = RectSubMode::VAPOR;
    config_.threshold_heads_end = 1.2f;
    config_.threshold_body_end = 0.3f;
    config_.delta_hysteresis = 0.1f;

    // distillation
    config_.dist_target_temp = 78.0f;
    config_.dist_temp_tolerance = 0.5f;

    // PID куба (нагрев)
    config_.pid_cube.kp = 2.0f;
    config_.pid_cube.ki = 0.5f;
    config_.pid_cube.kd = 1.0f;
    config_.pid_cube.out_min = 0.0f;
    config_.pid_cube.out_max = 100.0f;
    config_.pid_cube.integral_max = 50.0f;

    // PID охладителя (ректификация — дефлегматор)
    config_.pid_cooler.kp = 3.0f;
    config_.pid_cooler.ki = 0.3f;
    config_.pid_cooler.kd = 1.5f;
    config_.pid_cooler.out_min = 0.0f;
    config_.pid_cooler.out_max = 100.0f;
    config_.pid_cooler.integral_max = 50.0f;

    // PID охладителя (дистилляция — конденсатор, новое v1.1)
    config_.pid_dist_cooler.kp = 3.0f;
    config_.pid_dist_cooler.ki = 0.3f;
    config_.pid_dist_cooler.kd = 1.5f;
    config_.pid_dist_cooler.out_min = 0.0f;
    config_.pid_dist_cooler.out_max = 100.0f;
    config_.pid_dist_cooler.integral_max = 50.0f;

    // mqtt
    config_.mqtt_enabled = false;
    config_.mqtt_host[0] = '\0';
    config_.mqtt_port = 1883;
    config_.mqtt_user[0] = '\0';
    config_.mqtt_pass[0] = '\0';
    strncpy(config_.mqtt_topic_root, "distill", MAX_TOPIC_LEN - 1);

    // logging
    strncpy(config_.log_path, "/log", MAX_PATH_LEN - 1);
    config_.log_max_files = LOG_MAX_FILES;
    config_.log_rotation_mb = LOG_ROTATION_MB;

    // tft
    config_.tft_enabled = false;
    config_.tft_controller = TFTController::ILI9341;
    config_.tft_spi_clk = 40;
    config_.tft_spi_mosi = 23;
    config_.tft_spi_miso = 19;
    config_.tft_spi_cs = 5;
    config_.tft_spi_dc = 21;
    config_.tft_spi_rst = 18;
    config_.tft_backlight = 4;

    Serial.println("[SETTINGS] Defaults applied");
}

// ============================================================================
// validate
// ============================================================================

bool SettingsManager::validate() {
    // heads_end должно быть > body_end (ΔT уменьшается при переходе к телу)
    if (config_.threshold_heads_end <= config_.threshold_body_end) {
        Serial.println("[SETTINGS] Validation: heads_end <= body_end");
        return false;
    }

    // Гистерезис > 0
    if (config_.delta_hysteresis <= 0.0f) {
        Serial.println("[SETTINGS] Validation: hysteresis <= 0");
        return false;
    }

    // PIN-ы в допустимом диапазоне ESP32-S3 (0-48)
    if (config_.pin_pwm > 48) {
        Serial.println("[SETTINGS] Validation: pin_pwm > 48");
        return false;
    }
    if (config_.pin_cooler > 48) {
        Serial.println("[SETTINGS] Validation: pin_cooler > 48");
        return false;
    }

    // Количество сенсоров
    if (config_.sensor_count > MAX_SENSORS) {
        Serial.println("[SETTINGS] Validation: sensor_count > MAX");
        return false;
    }

    // MQTT порт
    if (config_.mqtt_enabled && config_.mqtt_port == 0) {
        Serial.println("[SETTINGS] Validation: mqtt_port == 0");
        return false;
    }

    // Температура дистилляции
    if (config_.dist_target_temp < 50.0f || config_.dist_target_temp > 100.0f) {
        Serial.println("[SETTINGS] Validation: dist_target_temp out of range");
        return false;
    }

    return true;
}
