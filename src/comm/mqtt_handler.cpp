/**
 * @file mqtt_handler.cpp
 * @brief Реализация MQTT клиента с Home Assistant Discovery.
 *
 * Архитектура:
 * 1. begin() — инициализация PubSubClient, но НЕ подключение сразу
 * 2. poll() — пытается подключиться каждые 5 сек, вызывает client.loop()
 * 3. При подключении — publishDiscovery() (один раз)
 * 4. publishSnapshot() — публикация телеметрии каждые 5 сек
 *
 * Home Assistant Discovery:
 * - Sensor: устройство с state_topic, unit_of_measurement, device_class
 * - Switch: устройство с command_topic и state_topic
 * - Number: устройство с command_topic, min, max, step
 * - Select: устройство с command_topic и options
 *
 * Все устройства группируются в device: {identifiers, name, manufacturer, model}
 */

#include "mqtt_handler.h"
#include "sensors/ds18b20_manager.h"
#include "control/heater_interface.h"
#include "core/state_machine.h"
#include "core/settings.h"
#include <ArduinoJson.h>

// ============================================================================
// Singleton
// ============================================================================

MQTTHandler& MQTTHandler::getInstance() {
    static MQTTHandler instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool MQTTHandler::begin(SettingsManager& settings, DS18B20Manager& sensors, IHeater& heater) {
    settings_ = &settings;
    sensors_ = &sensors;
    heater_ = &heater;

    const SystemConfig& cfg = settings.getConfig();

    if (!cfg.mqtt_enabled || cfg.mqtt_host[0] == '\0') {
        Serial.println("[MQTT] Disabled in config");
        enabled_ = false;
        return false;
    }

    enabled_ = true;
    strncpy(topic_root_, cfg.mqtt_topic_root, MAX_TOPIC_LEN - 1);

    // Создаём WiFi и MQTT клиенты
WiFiClient* wifiClient = new WiFiClient();
    client_ = new PubSubClient(*wifiClient);
    client_->setServer(cfg.mqtt_host, cfg.mqtt_port);
    client_->setCallback(MQTTHandler::onMqttMessage);
    client_->setBufferSize(4096);

    Serial.printf("[MQTT] Initialized: host=%s:%d, root=%s, buffer=4096\n",
                  cfg.mqtt_host, cfg.mqtt_port, topic_root_);
    return true;
}

// ============================================================================
// connect
// ============================================================================

bool MQTTHandler::connect() {
    if (!enabled_ || !client_) return false;

    const SystemConfig& cfg = settings_->getConfig();

    // Сбрасываем флаг Discovery при новом подключении
    discovery_sent_ = false;

    // Формируем client_id
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "rectcontroller_%06X",
             (unsigned int)ESP.getEfuseMac() & 0xFFFFFF);

    Serial.printf("[MQTT] Connecting to %s:%d ...\n", cfg.mqtt_host, cfg.mqtt_port);

    bool result;
    if (cfg.mqtt_user[0] != '\0') {
        result = client_->connect(client_id, cfg.mqtt_user, cfg.mqtt_pass);
    } else {
        result = client_->connect(client_id);
    }

    if (result) {
        Serial.println("[MQTT] Connected");
        connected_ = true;

        // Подписка на команды
        char sub_topic[128];
        snprintf(sub_topic, sizeof(sub_topic), "%s/command/#", topic_root_);
        client_->subscribe(sub_topic);
        Serial.printf("[MQTT] Subscribed to %s\n", sub_topic);

        // Home Assistant Discovery
        publishDiscovery();
    } else {
        Serial.printf("[MQTT] Connection failed, rc=%d\n", client_->state());
    }

    return result;
}

// ============================================================================
// publishDiscovery
// ============================================================================

void MQTTHandler::publishDiscovery() {
    if (discovery_sent_) return;

    char topic[128];
    char payload[1536];  // Увеличен для всех сущностей
    const SystemConfig& cfg = settings_->getConfig();

    // Базовая информация об устройстве
    // Все устройства группируются под одним device

    // --- Сенсоры температур ---
    const char* roles[] = {"T_column_top", "T_head_selection", "T_body_selection", "T_cooler"};
    const char* labels[] = {"Верх колонны", "Узел отбора голов", "Узел отбора тела", "Охлаждение"};

    for (int i = 0; i < 4; i++) {
        snprintf(topic, sizeof(topic), "homeassistant/sensor/distill/%s/config", roles[i]);

        JsonDocument doc;
        doc["name"] = labels[i];
        doc["state_topic"] = String(topic_root_) + "/sensors/" + roles[i];
        doc["unit_of_measurement"] = "°C";
        doc["device_class"] = "temperature";
        doc["state_class"] = "measurement";
        doc["value_template"] = "{{ value_json.temp }}";
        doc["unique_id"] = String("distill_") + roles[i];

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    // --- Фаза ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/distill/phase/config");
    {
        JsonDocument doc;
        doc["name"] = "Фаза";
        doc["state_topic"] = String(topic_root_) + "/status/phase";
        doc["unique_id"] = "distill_phase";

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    // --- Мощность нагрева ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/distill/heater_power/config");
    {
        JsonDocument doc;
        doc["name"] = "Мощность нагрева";
        doc["state_topic"] = String(topic_root_) + "/status/heater_power";
        doc["unit_of_measurement"] = "%";
        doc["state_class"] = "measurement";
        doc["unique_id"] = "distill_heater_power";

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    // --- ΔT ---
    snprintf(topic, sizeof(topic), "homeassistant/sensor/distill/delta_t/config");
    {
        JsonDocument doc;
        doc["name"] = "ΔT";
        doc["state_topic"] = String(topic_root_) + "/status/delta_t";
        doc["unit_of_measurement"] = "°C";
        doc["device_class"] = "temperature";
        doc["state_class"] = "measurement";
        doc["unique_id"] = "distill_delta_t";

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    // --- Switch: Heater Enable ---
    snprintf(topic, sizeof(topic), "homeassistant/switch/distill/heater_enable/config");
    {
        JsonDocument doc;
        doc["name"] = "Нагрев вкл/выкл";
        doc["state_topic"] = String(topic_root_) + "/status/heater_enabled";
        doc["command_topic"] = String(topic_root_) + "/command/heater_enable";
        doc["unique_id"] = "distill_heater_enable";

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    // --- Button: Emergency Stop ---
    snprintf(topic, sizeof(topic), "homeassistant/button/distill/emergency_stop/config");
    {
        JsonDocument doc;
        doc["name"] = "Аварийная остановка";
        doc["command_topic"] = String(topic_root_) + "/command/emergency_stop";
        doc["unique_id"] = "distill_emergency_stop";

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    // --- Number: PID куба Kp, Ki, Kd ---
    const char* pid_names[] = {"pid_cube_kp", "pid_cube_ki", "pid_cube_kd",
                                "pid_cooler_kp", "pid_cooler_ki", "pid_cooler_kd"};
    const char* pid_labels[] = {"PID куба Kp", "PID куба Ki", "PID куба Kd",
                                 "PID охладителя Kp", "PID охладителя Ki", "PID охладителя Kd"};

    for (int i = 0; i < 6; i++) {
        snprintf(topic, sizeof(topic), "homeassistant/number/distill/%s/config", pid_names[i]);

        JsonDocument doc;
        doc["name"] = pid_labels[i];
        doc["command_topic"] = String(topic_root_) + "/command/set_pid_" + String(pid_names[i]);
        doc["min"] = 0.0;
        doc["max"] = 100.0;
        doc["step"] = 0.1;
        doc["unique_id"] = String("distill_") + pid_names[i];

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    // --- Select: Фаза (ручная установка) ---
    snprintf(topic, sizeof(topic), "homeassistant/select/distill/phase/config");
    {
        JsonDocument doc;
        doc["name"] = "Фаза (ручная)";
        doc["command_topic"] = String(topic_root_) + "/command/set_phase";
        doc["unique_id"] = "distill_phase_select";

        JsonArray opts = doc["options"].to<JsonArray>();
        opts.add("idle");
        opts.add("heating");
        opts.add("heads");
        opts.add("body");
        opts.add("tails");
        opts.add("finish");

        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"] = "rectcontroller_s3";
        dev["name"] = cfg.device_name;
        dev["manufacturer"] = "Custom";
        dev["model"] = "RectController S3";

        serializeJson(doc, payload, sizeof(payload));
        publishDiscoveryConfig(topic, payload);
    }

    discovery_sent_ = true;
    Serial.println("[MQTT] Home Assistant Discovery completed");
    Serial.printf("[MQTT] Payload buffer size: %d bytes\n", sizeof(payload));
}

// ============================================================================
// publishDiscoveryConfig
// ============================================================================

void MQTTHandler::publishDiscoveryConfig(const char* topic, const char* payload) {
    if (!client_ || !client_->connected()) {
        Serial.printf("[MQTT] Discovery SKIP (not connected): %s\n", topic);
        return;
    }

    Serial.printf("[MQTT] Discovery publish: %s\n", topic);
    bool ok = client_->publish(topic, payload, true);
    Serial.printf("[MQTT] Result: %s, size: %d\n", ok ? "OK" : "FAIL", strlen(payload));
    delay(50);
}

// ============================================================================
// publishSnapshot
// ============================================================================

void MQTTHandler::publishSnapshot(const SystemSnapshot& snapshot) {
    if (!enabled_ || !client_ || !client_->connected()) return;

    char topic[128];
    char payload[128];

    // Сенсоры температур
    for (uint8_t i = 0; i < snapshot.sensor_count; i++) {
        const SensorData& s = snapshot.sensors[i];
        if (!s.present) continue;

        const char* role = roleToString(s.role);
        snprintf(topic, sizeof(topic), "%s/sensors/%s", topic_root_, role);

        JsonDocument doc;
        doc["temp"] = s.temp_corrected;
        doc["raw"] = s.temp_raw;
        doc["offset"] = s.offset;
        doc["present"] = s.present;

        serializeJson(doc, payload, sizeof(payload));
        client_->publish(topic, payload);
    }

    // Статус: фаза
    snprintf(topic, sizeof(topic), "%s/status/phase", topic_root_);
    client_->publish(topic, phaseToString(snapshot.phase));

    // Статус: мощность
    snprintf(topic, sizeof(topic), "%s/status/heater_power", topic_root_);
    snprintf(payload, sizeof(payload), "%d", snapshot.heater_power);
    client_->publish(topic, payload);

    // Статус: heater_enabled
    snprintf(topic, sizeof(topic), "%s/status/heater_enabled", topic_root_);
    client_->publish(topic, snapshot.heater_enabled ? "ON" : "OFF");

    // Статус: delta_t
    snprintf(topic, sizeof(topic), "%s/status/delta_t", topic_root_);
    snprintf(payload, sizeof(payload), "%.2f", snapshot.delta_t);
    client_->publish(topic, payload);

    // Статус: calibrating
    snprintf(topic, sizeof(topic), "%s/status/calibrating", topic_root_);
    client_->publish(topic, snapshot.calibrating ? "ON" : "OFF");

    // Статус: error
    snprintf(topic, sizeof(topic), "%s/status/error", topic_root_);
    client_->publish(topic, (snapshot.error_count > 0) ? "ON" : "OFF");
}

// ============================================================================
// poll
// ============================================================================

void MQTTHandler::poll() {
    if (!enabled_ || !client_) return;

    uint32_t now = millis();

    // Поддержание соединения
    if (client_->connected()) {
        client_->loop();
    } else {
        // Попытка переподключения
        if (now - reconnect_timer_ >= RECONNECT_INTERVAL_MS) {
            reconnect_timer_ = now;
            connect();
        }
    }
}

// ============================================================================
// isConnected
// ============================================================================

bool MQTTHandler::isConnected() const {
    return client_ && client_->connected();
}

// ============================================================================
// onMqttMessage (статический callback)
// ============================================================================

void MQTTHandler::onMqttMessage(char* topic, byte* payload, unsigned int length) {
    // Преобразуем payload в null-terminated строку
    char payload_str[256];
    unsigned int copy_len = (length < sizeof(payload_str) - 1) ? length : sizeof(payload_str) - 1;
    memcpy(payload_str, payload, copy_len);
    payload_str[copy_len] = '\0';

    getInstance().handleCommand(topic, payload_str, copy_len);
}

// ============================================================================
// handleCommand
// ============================================================================

void MQTTHandler::handleCommand(const char* topic, const char* payload, size_t length) {
    // Определяем команду по топику
    String topic_str(topic);
    String root = String(topic_root_) + "/command/";

    if (topic_str == root + "emergency_stop") {
        StateMachine::getInstance().emergencyStop();
        Serial.println("[MQTT] Emergency stop via MQTT");
        return;
    }

    if (topic_str == root + "reset_error") {
        StateMachine::getInstance().resetError();
        Serial.println("[MQTT] Reset error via MQTT");
        return;
    }

    if (topic_str == root + "calibrate") {
        StateMachine::getInstance().startCalibration();
        Serial.println("[MQTT] Calibrate via MQTT");
        return;
    }

    if (topic_str == root + "start") {
        StateMachine::getInstance().start();
        Serial.println("[MQTT] Start via MQTT");
        return;
    }

    if (topic_str == root + "set_power") {
        uint8_t power = atoi(payload);
        heater_->setPower(power);
        Serial.printf("[MQTT] Set power: %d\n", power);
        return;
    }

    if (topic_str == root + "heater_enable") {
        bool enable = (strcmp(payload, "ON") == 0 || strcmp(payload, "true") == 0 || strcmp(payload, "1") == 0);
        heater_->enableHeating(enable);
        Serial.printf("[MQTT] Heater enable: %d\n", enable);
        return;
    }

    if (topic_str.startsWith(root + "set_phase")) {
        DistillPhase phase = stringToPhase(payload);
        StateMachine::getInstance().setPhase(phase);
        Serial.printf("[MQTT] Set phase: %s\n", payload);
        return;
    }

    // PID параметры: set_pid_cube_kp, set_pid_cooler_ki, и т.д.
    if (topic_str.startsWith(root + "set_pid_")) {
        String suffix = topic_str.substring(root.length() + 8); // после "set_pid_"
        float value = atof(payload);

        const SystemConfig& cfg = settings_->getConfig();
        PIDParams params;

        if (suffix.startsWith("cube_")) {
            params = cfg.pid_cube;
            if (suffix == "cube_kp") params.kp = value;
            else if (suffix == "cube_ki") params.ki = value;
            else if (suffix == "cube_kd") params.kd = value;
            settings_->savePIDCubeParams(params);
        } else if (suffix.startsWith("cooler_")) {
            params = cfg.pid_cooler;
            if (suffix == "cooler_kp") params.kp = value;
            else if (suffix == "cooler_ki") params.ki = value;
            else if (suffix == "cooler_kd") params.kd = value;
            settings_->savePIDCoolerParams(params);
        }

        Serial.printf("[MQTT] PID %s = %.2f\n", suffix.c_str(), value);
        return;
    }

    Serial.printf("[MQTT] Unknown command topic: %s\n", topic);
}
