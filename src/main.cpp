/**
 * @file main.cpp
 * @brief Главный файл прошивки: инициализация и главный цикл.
 *
 * Использует WiFiPortal из lib/ (esp32-starter-kit):
 * - AP + STA + Captive Portal
 *
 * v1.1: добавлена поддержка двух режимов (distillation/rectification),
 *       детальное логирование инициализации, новые поля конфига.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

// WiFi Portal из esp32-starter-kit
#include "WiFiPortal.h"

// Наши модули
#include "core/config.h"
#include "core/settings.h"
#include "core/state_machine.h"
#include "sensors/ds18b20_manager.h"
#include "sensors/calibration.h"
#include "control/heater_interface.h"
#include "control/electric_heater.h"
#include "control/gas_heater.h"
#include "control/phase_selector.h"
#include "storage/fs_manager.h"
#include "storage/logger.h"
#include "comm/espnow_handler.h"
#include "comm/web_api.h"
#include "comm/mqtt_handler.h"
#include "comm/ai_bridge.h"
#ifdef TFT_ENABLED
#include "ui/tft_display.h"
#endif

#define FIRMWARE_VERSION "1.1.0"
#define DEVICE_NAME "RectController-S3"

// Глобальные переменные
uint32_t g_start_time_ms = 0;
SystemSnapshot g_snapshot = {};
bool mqttInitialized = false;

// Таймеры
uint32_t g_last_telemetry = 0;
uint32_t g_last_status = 0;
uint32_t g_last_log = 0;
uint32_t g_last_mqtt = 0;

static constexpr uint32_t TELEMETRY_INTERVAL = 500;
static constexpr uint32_t STATUS_INTERVAL = 30000;
static constexpr uint32_t MQTT_PUBLISH_INTERVAL = 5000;

// Forward declarations
void updateSnapshot(uint32_t now);
void printStatus();
void loadConfig();

// ============================================================
// WiFi события
// ============================================================
void onWiFiEvent(WiFiPortal* portal, const char* event) {
    Serial.printf("[WiFi] Event: %s\n", event);

    if (strcmp(event, "connected") == 0) {
        Serial.printf("[WiFi] Connected! IP: %s\n", portal->getLocalIP().c_str());

        // Инициализируем MQTT при подключении к WiFi
        if (!mqttInitialized && SettingsManager::getInstance().getConfig().mqtt_enabled) {
            const auto& cfg = SettingsManager::getInstance().getConfig();
            IHeater* heater = (cfg.heater_type == HeaterType::ELECTRIC)
                ? (IHeater*)&ElectricHeater::getInstance()
                : (IHeater*)&GasHeater::getInstance();

            MQTTHandler::getInstance().begin(
                SettingsManager::getInstance(),
                DS18B20Manager::getInstance(),
                *heater
            );
            mqttInitialized = true;
            Serial.println("[MQTT] Initialized");
        }
    }
}

// ============================================================
// Загрузка конфигурации
// ============================================================
void loadConfig() {
    Serial.println("[CONFIG] Loading settings.json...");
    bool loaded = SettingsManager::getInstance().begin("/settings.json");

    if (loaded) {
        Serial.println("[CONFIG] ✓ Loaded from /settings.json");
    } else {
        Serial.println("[CONFIG] ⚠ Using defaults");
    }

    const auto& cfg = SettingsManager::getInstance().getConfig();

    // Лог ключевых настроек
    Serial.printf("[CONFIG]   Mode: %s\n", modeToString(cfg.mode));
    Serial.printf("[CONFIG]   Heater: %s\n", heaterTypeToString(cfg.heater_type));
    Serial.printf("[CONFIG]   Sensors: %d\n", cfg.sensor_count);
    Serial.printf("[CONFIG]   PWM pin: %d, Cooler pin: %d\n", cfg.pin_pwm, cfg.pin_cooler);
    Serial.printf("[CONFIG]   Cooler type: %s\n", coolerTypeToString(cfg.cooler_type));
    Serial.printf("[CONFIG]   Gas control: %s\n", gasControlModeToString(cfg.gas_control_mode));

    // WiFi credentials
    if (cfg.wifi_ssid[0] != '\0') {
        wifiPortal.setStaCredentials(cfg.wifi_ssid, cfg.wifi_pass);
        Serial.printf("[CONFIG]   WiFi SSID: %s\n", cfg.wifi_ssid);
    } else {
        Serial.println("[CONFIG]   WiFi SSID: (empty, AP mode only)");
    }

    // AP настройки — открытая сеть (без пароля) для captive portal
    wifiPortal.setApCredentials("RectController_AP", "");  // БЕЗ пароля!
    wifiPortal.setApBehavior(ApBehavior::ALWAYS);
    wifiPortal.setApChannel(6);  // Фиксированный канал для стабильности
    wifiPortal.setApIP(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    wifiPortal.setTxPower(WIFI_POWER_8_5dBm);
    wifiPortal.setHostname(DEVICE_NAME);
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n╔══════════════════════════════════════════╗");
    Serial.println("║   RectController S3 v" FIRMWARE_VERSION "         ║");
    Serial.println("╚══════════════════════════════════════════╝");
    Serial.printf("[BOOT] Chip: %s rev.%d, %d cores, %d MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getChipCores(), ESP.getCpuFreqMHz());
    Serial.printf("[BOOT] Flash: %d MB, PSRAM: %d MB\n",
                  ESP.getFlashChipSize() / (1024 * 1024),
                  ESP.getPsramSize() / (1024 * 1024));
    Serial.printf("[BOOT] Free heap: %u bytes\n", ESP.getFreeHeap());

    // 1. LittleFS + FSManager
    Serial.println("\n[1/11] FS: Initializing LittleFS...");
    if (!FSManager::getInstance().begin()) {
        Serial.println("[1/11] ✗ FS ERROR! System halted.");
        while (1) delay(1000);
    }
    Serial.printf("[1/11] ✓ FS OK (total: %u, used: %u bytes)\n",
                  LittleFS.totalBytes(), LittleFS.usedBytes());

    // 2. Загрузка конфигурации
    Serial.println("\n[2/11] CONFIG: Loading...");
    loadConfig();
    Serial.println("[2/11] ✓ Config loaded");

    // 3. WiFi Portal (STA + AP fallback)
    Serial.println("\n[3/11] WiFi: Initializing WiFiPortal...");
    wifiPortal.onEvent(onWiFiEvent);
    wifiPortal.begin();
    Serial.println("[3/11] ✓ WiFi OK (connecting / AP starting)");

    // 4. Сенсоры DS18B20
    Serial.println("\n[4/11] SENSORS: Initializing DS18B20...");
    if (DS18B20Manager::getInstance().begin(4, SettingsManager::getInstance())) {
        Serial.printf("[4/11] ✓ Sensors OK (%d found)\n",
                      DS18B20Manager::getInstance().getSensorCount());
    } else {
        Serial.println("[4/11] ⚠ No sensors found");
    }

    // 5. Нагреватель
    Serial.println("\n[5/11] HEATER: Initializing...");
    const auto& cfg = SettingsManager::getInstance().getConfig();
    IHeater* heater = nullptr;

    if (cfg.heater_type == HeaterType::ELECTRIC) {
        if (ElectricHeater::getInstance().begin(cfg.pin_pwm, cfg.pin_cooler,
                                               cfg.pid_cube, cfg.pid_cooler,
                                               cfg.pid_dist_cooler)) {
            // Устанавливаем режим и тип охладителя
            ElectricHeater::getInstance().setOperationMode(cfg.mode);
            ElectricHeater::getInstance().setCoolerType(cfg.cooler_type);
            Serial.println("[5/11] ✓ Electric heater OK");
            heater = &ElectricHeater::getInstance();
        } else {
            Serial.println("[5/11] ✗ Electric heater ERROR");
        }
    } else {
        if (GasHeater::getInstance().begin(cfg.espnow_mac_gas)) {
            GasHeater::getInstance().setOperationMode(cfg.mode);
            GasHeater::getInstance().setControlMode(cfg.gas_control_mode);
            Serial.println("[5/11] ✓ Gas heater OK");
            heater = &GasHeater::getInstance();
        } else {
            Serial.println("[5/11] ✗ Gas heater ERROR");
        }
    }

    // 6. Phase Selector
    Serial.println("\n[6/11] PHASE: Initializing phase selector...");
    PhaseSelector::getInstance().begin(SettingsManager::getInstance());
    Serial.printf("[6/11] ✓ Phase OK (thresholds: %.2f / %.2f, hyst: %.2f)\n",
                  cfg.threshold_heads_end, cfg.threshold_body_end, cfg.delta_hysteresis);

    // 7. State Machine
    Serial.println("\n[7/11] STATE: Initializing state machine...");
    if (heater != nullptr) {
        StateMachine::getInstance().begin(
            *heater,
            DS18B20Manager::getInstance(),
            SettingsManager::getInstance()
        );
        Serial.printf("[7/11] ✓ State machine OK (mode: %s)\n", modeToString(cfg.mode));
    } else {
        Serial.println("[7/11] ✗ State machine skipped (no heater)");
    }

    // 8. Logger
    Serial.println("\n[8/11] LOGGER: Initializing...");
    if (Logger::getInstance().begin(SettingsManager::getInstance())) {
        Serial.println("[8/11] ✓ Logger OK");
    } else {
        Serial.println("[8/11] ⚠ Logger disabled");
    }

    // 9. Web API (HTTP + WebSocket)
    Serial.println("\n[9/11] WEB-API: Initializing...");
    if (heater != nullptr) {
        WebAPI::getInstance().begin(
            80,
            DS18B20Manager::getInstance(),
            *heater,
            PhaseSelector::getInstance(),
            SettingsManager::getInstance(),
            Logger::getInstance()
        );
        Serial.println("[9/11] ✓ Web API OK");
    } else {
        Serial.println("[9/11] ⚠ Web API skipped");
    }

    // 10. AI Bridge
    Serial.println("\n[10/11] AI: Initializing...");
    AIBridge::getInstance().begin(
        StateMachine::getInstance(),
        DS18B20Manager::getInstance(),
        SettingsManager::getInstance()
    );
    Serial.println("[10/11] ✓ AI Bridge OK");

    // 11. TFT (опционально)
#ifdef TFT_ENABLED
    Serial.println("\n[11/11] TFT: Initializing display...");
    if (cfg.tft_enabled) {
        if (!TFTDisplay::getInstance().begin()) {
            Serial.println("[11/11] ⚠ TFT disabled");
        } else {
            Serial.println("[11/11] ✓ TFT OK");
        }
    } else {
        Serial.println("[11/11] TFT: not enabled in config");
    }
#else
    Serial.println("\n[11/11] TFT: not compiled (TFT_ENABLED not defined)");
#endif

    // Глобальные переменные
    g_start_time_ms = millis();
    g_snapshot.mode = cfg.mode;
    g_snapshot.phase = DistillPhase::IDLE;
    g_snapshot.heater_type = cfg.heater_type;
    g_snapshot.dist_target_temp = cfg.dist_target_temp;

    Serial.println("\n[BOOT] ═══════ System ready! ═══════\n");
    Serial.printf("[BOOT] Free heap after init: %u bytes\n", ESP.getFreeHeap());
}

// ============================================================
// LOOP
// ============================================================
void loop() {
    uint32_t now = millis();

    // 1. WiFi Portal (DNS, reconnect, AP behavior)
    wifiPortal.loop();

    // 2. Опрос сенсоров (неблокирующий)
    DS18B20Manager::getInstance().poll();

    // 3. Обновление state machine
    StateMachine::getInstance().update(g_snapshot);

    // 4. Обновление снапшота
    updateSnapshot(now);

    // 5. Отправка телеметрии через WebSocket
    if (now - g_last_telemetry >= TELEMETRY_INTERVAL) {
        g_last_telemetry = now;
        WebAPI::getInstance().broadcastTelemetry(g_snapshot);
    }

    // 5.1. Обработка WebSocket клиентов
    WebAPI::getInstance().poll();

    // 6. Логирование
    Logger::getInstance().poll();
    if (now - g_last_log >= 5000) {
        g_last_log = now;
        Logger::getInstance().log(g_snapshot);
    }

    // 7. MQTT
    if (mqttInitialized) {
        if (now - g_last_mqtt >= MQTT_PUBLISH_INTERVAL) {
            g_last_mqtt = now;
            MQTTHandler::getInstance().poll();
            MQTTHandler::getInstance().publishSnapshot(g_snapshot);
        }
    }

    // 8. Settings watcher
    SettingsManager::getInstance().watch();

    // 9. Периодический статус в Serial
    if (now - g_last_status >= STATUS_INTERVAL) {
        g_last_status = now;
        printStatus();
    }

    // Отдаём управление WiFi стеку
    delay(2);
}

// ============================================================
// Обновление снапшота
// ============================================================
void updateSnapshot(uint32_t now) {
    const auto& cfg = SettingsManager::getInstance().getConfig();

    g_snapshot.mode = cfg.mode;
    g_snapshot.uptime_sec = (now - g_start_time_ms) / 1000;
    g_snapshot.timestamp_ms = now;

    // Копируем данные сенсоров
    const SensorData* sensors = DS18B20Manager::getInstance().getSensors();
    uint8_t count = DS18B20Manager::getInstance().getSensorCount();
    g_snapshot.sensor_count = count;
    for (uint8_t i = 0; i < count && i < MAX_SENSORS; i++) {
        g_snapshot.sensors[i] = sensors[i];
    }

    // Нагрев
    if (cfg.heater_type == HeaterType::ELECTRIC) {
        g_snapshot.heater_power = ElectricHeater::getInstance().getPower();
        g_snapshot.heater_enabled = ElectricHeater::getInstance().isHeatingEnabled();
        g_snapshot.pid_out_cube = ElectricHeater::getInstance().getHeaterPIDOutput();
        g_snapshot.pid_out_cooler = ElectricHeater::getInstance().getCoolerPIDOutput();
    } else {
        g_snapshot.heater_power = GasHeater::getInstance().getPower();
        g_snapshot.heater_enabled = GasHeater::getInstance().isHeatingEnabled();
        g_snapshot.pid_out_cube = StateMachine::getInstance().getPIDCubeOut();
        g_snapshot.pid_out_cooler = StateMachine::getInstance().getPIDCoolerOut();
    }

    // Фаза и ΔT
    g_snapshot.phase = StateMachine::getInstance().getPhase();
    g_snapshot.delta_t = PhaseSelector::getInstance().getDeltaT();
    g_snapshot.calibrating = DS18B20Manager::getInstance().isCalibrating();

    // Distillation-specific
    if (cfg.mode == OperationMode::DISTILLATION) {
        g_snapshot.dist_target_temp = cfg.dist_target_temp;
        const SensorData* s_boiler = DS18B20Manager::getInstance().getSensorByRole(SensorRole::boiler);
        if (s_boiler == nullptr) {
            s_boiler = DS18B20Manager::getInstance().getSensorByRole(SensorRole::column_top);
        }
        if (s_boiler != nullptr && s_boiler->present) {
            g_snapshot.dist_current_temp = s_boiler->temp_corrected;
        }
    }
}

// ============================================================
// Печать статуса
// ============================================================
void printStatus() {
    const auto& cfg = SettingsManager::getInstance().getConfig();

    Serial.println("\n========== STATUS ==========");
    Serial.printf("Mode: %s\n", modeToString(cfg.mode));
    Serial.printf("Phase: %s\n", phaseToString(g_snapshot.phase));
    Serial.printf("Delta T: %.2f C\n", g_snapshot.delta_t);
    Serial.printf("Heater: %s, %d%%, enabled=%d\n",
                  heaterTypeToString(g_snapshot.heater_type),
                  g_snapshot.heater_power,
                  g_snapshot.heater_enabled);
    Serial.printf("PID Cube: %.1f%%, Cooler: %.1f%%\n",
                  g_snapshot.pid_out_cube, g_snapshot.pid_out_cooler);
    Serial.printf("Cooler type: %s\n", coolerTypeToString(cfg.cooler_type));
    Serial.printf("Sensors: %d\n", g_snapshot.sensor_count);
    for (uint8_t i = 0; i < g_snapshot.sensor_count; i++) {
        Serial.printf("  [%d] %s (%s): %.2f C | raw: %.2f | off: %.2f | active: %d | present: %d\n",
                      i,
                      roleToString(g_snapshot.sensors[i].role),
                      g_snapshot.sensors[i].name,
                      g_snapshot.sensors[i].temp_corrected,
                      g_snapshot.sensors[i].temp_raw,
                      g_snapshot.sensors[i].offset,
                      g_snapshot.sensors[i].active,
                      g_snapshot.sensors[i].present);
    }
    Serial.printf("Uptime: %lu sec\n", g_snapshot.uptime_sec);
    Serial.printf("WiFi: %s, IP: %s\n",
                  wifiPortal.isConnected() ? "connected" : "disconnected",
                  wifiPortal.getLocalIP().c_str());
    Serial.printf("AP: %s, clients: %d\n",
                  wifiPortal.isApActive() ? "active" : "inactive",
                  wifiPortal.getApClientCount());
    Serial.printf("Heap: %u bytes, PSRAM: %u bytes free\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.printf("MQTT: %s\n", mqttInitialized ? "initialized" : "disabled");
    Serial.println("============================\n");
}
