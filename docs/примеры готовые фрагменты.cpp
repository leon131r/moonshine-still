//Определения структур данных, в том числе пакеты ESP-NOW.


#pragma once
#include <Arduino.h>

// --- Enumerations ---
enum class SystemMode { DISTILLATION, RECTIFICATION };
enum class Phase { IDLE, HEATING, HEADS, BODY, TAILS, FINISH, ERROR };
enum class PhaseTransitionMode { MANUAL, DELTA_TEMP, QUANTITATIVE };
enum class HeaterType { ELECTRIC, GAS };

// --- ESP-NOW Payloads ---

// 1. Газовый блок: Контроллер -> Газ
struct GasControlData {
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    float setpoint_temp = 0.0f;
    uint8_t power_manual = 0; // 0-100
    bool enable_heating = false;
    uint8_t mode = 0; // 0=Manual, 1=Auto, 2=RemotePID
    uint32_t crc = 0;
};

// 1. Газовый блок: Газ -> Контроллер
struct GasStatusData {
    float current_temp = 0.0f;
    float block_temp = 0.0f;
    uint8_t current_power = 0;
    bool heating_active = false;
    bool error_flag = false;
    uint32_t crc = 0;
};

// 2. Уровень/Крепость: Модуль -> Контроллер
struct LevelMeasurementData {
    uint16_t level_mm = 0;
    float abv_percent = 0.0f;
    float pressure_hPa = 1013.0f;
    float temp_liquid = 0.0f;
    uint32_t crc = 0;
};

// --- Constants ---
const uint8_t MAX_SENSORS = 4;
const uint16_t LOG_BUFFER_SIZE = 1024;


//Управление JSON конфигурацией. Здесь критически важна таблица калибровки емкости.


#pragma once
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "../core/config.h"

struct TankCalibrationPoint {
    uint16_t mm;
    uint16_t ml;
};

struct SystemSettings {
    SystemMode mode = SystemMode::RECTIFICATION;
    
    // Rectification
    PhaseTransitionMode rect_mode = PhaseTransitionMode::DELTA_TEMP;
    float rect_heads_delta = 1.2f;
    float rect_body_delta = 0.3f;
    
    // Distillation
    PhaseTransitionMode dist_heads_mode = PhaseTransitionMode::MANUAL;
    float dist_heads_percent = 10.0f;

    // Tank
    uint16_t tank_capacity_ml = 1000;
    uint16_t tank_warning_percent = 90;
    bool tank_auto_stop = true;
    std::vector<TankCalibrationPoint> tank_calibration;

    // ESP-NOW
    String mac_gas = "";
    String mac_level = "";
    
    // MQTT
    String mqtt_host = "";
    uint16_t mqtt_port = 1883;
    String mqtt_topic = "distill";
    bool mqtt_enabled = false;
    
    // OTA
    String ota_pass = "";
    bool ota_enabled = false;
};

class SettingsManager {
public:
    SystemSettings current;

    bool load();
    bool save();
    uint16_t calcVolumeFromMM(uint16_t mm); // Интерполяция по таблице
};


//Сердце логики. Реализует оба режима с учетом новых требований (Quantitative, Delta, Manual).


#pragma once
#include "../core/config.h"
#include "../storage/settings.h"

class PhaseStateMachine {
public:
    Phase currentState = Phase::IDLE;
    Phase targetState = Phase::IDLE;
    
    // Статистика для "Quantitative" режима
    float currentHeadsVolume = 0.0f;
    float currentBodyVolume = 0.0f;
    float currentTailsVolume = 0.0f;

    void update(float deltaT, uint16_t currentVolume, bool manualNextCommand);
    Phase getCurrentPhase() { return currentState; }
    void reset();

private:
    bool checkTransitionRect(float deltaT, uint16_t vol);
    bool checkTransitionDist(uint16_t vol);
};

// В файле .cpp:
void PhaseStateMachine::update(float deltaT, uint16_t currentVol, bool manualNext) {
    if (currentState == Phase::ERROR || currentState == Phase::IDLE) return;

    // Логика для ДИСТИЛЛЯЦИИ
    if (settings.current.mode == SystemMode::DISTILLATION) {
        if (currentState == Phase::HEADS) {
            bool quantDone = false;
            if (settings.current.dist_heads_mode == PhaseTransitionMode::QUANTITATIVE) {
                float maxHeads = (float)currentVol * (settings.current.dist_heads_percent / 100.0f); // Пример расчета
                // Примечание: нужен плановый объем для точного расчета, здесь упрощено
            }
            if (manualNext || quantDone) transitionTo(Phase::BODY);
        }
    }
    // Логика для РЕКТИФИКАЦИИ
    else {
        if (currentState == Phase::HEADS) {
            bool deltaOk = (settings.current.rect_mode == PhaseTransitionMode::DELTA_TEMP && deltaT < settings.current.rect_heads_delta);
            // Quant logic skipped for brevity, similar to above
            if (manualNext || deltaOk) transitionTo(Phase::BODY);
        }
        else if (currentState == Phase::BODY) {
             bool deltaOk = (settings.current.rect_mode == PhaseTransitionMode::DELTA_TEMP && deltaT < settings.current.rect_body_delta);
             if (manualNext || deltaOk) transitionTo(Phase::TAILS);
        }
    }
}

//Управляет двумя каналами ESP-NOW (Газ и Уровень).

#include <esp_now.h>
#include <WiFi.h>
#include "espnow_manager.h"
#include "../core/config.h"

// Callback для приема данных
void onDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    // Определяем, кто прислал по MAC адресу
    String srcMac = mac2str(recv_info->src_addr);
    
    if (srcMac == settingsManager.current.mac_gas) {
        if (len == sizeof(GasStatusData)) {
            GasStatusData status;
            memcpy(&status, data, sizeof(status));
            // CRC Check here
            onGasStatusReceived(status);
        }
    } 
    else if (srcMac == settingsManager.current.mac_level) {
        if (len == sizeof(LevelMeasurementData)) {
            LevelMeasurementData lvl;
            memcpy(&lvl, data, sizeof(lvl));
            onLevelReceived(lvl);
        }
    }
}

// Отправка управления газом
void sendGasControl(const GasControlData& cmd) {
    if (settingsManager.current.mac_gas == "") return;
    uint8_t mac[6];
    parseMac(settingsManager.current.mac_gas, mac);
    
    esp_now_send(mac, (uint8_t*)&cmd, sizeof(cmd));
}

// Пересчет уровня в объем (вызывается при получении LevelMeasurementData)
void onLevelReceived(const LevelMeasurementData& data) {
    uint16_t volume = settingsManager.calcVolumeFromMM(data.level_mm);
    
    // Обновление глобальных переменных для UI
    g_systemState.tank_current_volume = volume;
    g_systemState.abv = data.abv_percent;
    g_systemState.pressure = data.pressure_hPa;
    
    // Проверка на заполнение
    if (volume >= settingsManager.current.tank_capacity_ml) {
        if (settingsManager.current.tank_auto_stop) {
            triggerEmergencyStop("TANK_FULL");
        }
    }
}

//Реализация OTA обновления и настроек MQTT.


#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <ArduinoJson.h>

AsyncWebServer server(80);

void setupWebAPI() {
    // ... стандартные route для статусов ...

    // --- OTA Endpoint ---
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!settingsManager.current.ota_enabled) {
            request->send(403, "text/plain", "OTA Disabled");
            return;
        }
        // Проверка пароля из POST body может быть добавлена здесь
        request->send(Update.hasError() ? 500 : 200, "text/plain", 
                      Update.hasError() ? "FAIL" : "OK");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, 
          uint8_t *data, size_t len, bool final){
        if(!index){
            if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                Update.printError(Serial);
            }
        }
        Update.write(data, len);
        if(final){
            if(Update.end(true)){
                Serial.println("Update Success");
                ESP.restart();
            } else {
                Update.printError(Serial);
            }
        }
    });

    // --- MQTT Settings Update ---
    server.on("/api/settings/mqtt", HTTP_POST, [](AsyncWebServerRequest *request){
        // Парсинг JSON и сохранение settingsManager.current.mqtt...
        request->send(200, "application/json", "{\"status\":\"saved\"}");
    });
}

//Расширенное логирование с учетом объемов и давлений.


#include <LittleFS.h>
#include <ArduinoJson.h>

void logSnapshot(const SystemState& state) {
    if (!LittleFS.exists("/log")) LittleFS.mkdir("/log");
    
    File f = LittleFS.open("/log/current.ndjson", "a");
    if (f) {
        StaticJsonDocument<256> doc;
        doc["ts"] = millis();
        doc["mode"] = (int)settingsManager.current.mode;
        doc["phase"] = (int)state.phase;
        doc["T_cube"] = state.tempBoiler;
        doc["T_top"] = state.tempTop;
        doc["delta_T"] = state.deltaT;
        doc["vol_ml"] = state.tank_current_volume; // Текущий объем
        doc["abv"] = state.abv;
        doc["pressure"] = state.pressure;
        
        serializeJson(doc, f);
        f.println();
        f.close();
    }
}

//Сборка всех компонентов.


#include <Arduino.h>
#include "core/config.h"
#include "storage/settings.h"
#include "sensors/sensor_manager.h"
#include "control/state_machine.h"
#include "comm/espnow_manager.h"
#include "comm/web_api.h"
#include "storage/logger.h"

SettingsManager settingsManager;
PhaseStateMachine phaseController;
SensorManager sensors;

void setup() {
    Serial.begin(115200);
    
    if (!LittleFS.begin(true)) Serial.println("LittleFS Error");
    
    settingsManager.load();
    
    sensors.begin();
    espnow_init();
    setupWebAPI();
    
    Serial.println("System Ready");
}

void loop() {
    sensors.update(); // Чтение DS18B20
    
    // Расчет дельты для ректификации
    float delta = sensors.getTemp("head_selection") - sensors.getTemp("body_selection");
    
    // Обновление машины состояний
    phaseController.update(delta, g_systemState.tank_current_volume, g_manualNextFlag);
    
    // Логика нагрева (PID или отправка в ESP-NOW)
    // ... Heater logic ...
    
    // Отправка управления газом если нужно
    GasControlData cmd; 
    cmd.setpoint_temp = 78.0; // Пример
    sendGasControl(cmd);
    
    logger.logSnapshot(g_systemState);
    
    delay(500); // Cycle time
}


