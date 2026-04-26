/**
 * @file web_api.cpp
 * @brief Реализация HTTP API + WebSocket.
 *
 * Архитектура:
 * - ESPAsyncWebServer обрабатывает HTTP и WebSocket асинхронно
 * - Callback-и вызываются из контекста сетевого стека (не ISR)
 * - Serial.printf допустим, но без delay()
 *
 * HTTP endpoints:
 *   GET  /api/status/snapshot → SystemSnapshot JSON
 *   GET  /api/log/latest?n=50 → NDJSON строки
 *   GET  /api/config          → настройки (JSON)
 *   POST /api/command         → команды
 *
 * WebSocket (/ws):
 *   Сервер → Клиент: {"type":"telemetry", ...}
 *   Клиент → Сервер: {"cmd":"set_power", "value":50}
 */

#include "comm/web_api.h"
#include "core/state_machine.h"
#include "sensors/ds18b20_manager.h"
#include "control/heater_interface.h"
#include "control/phase_selector.h"
#include "control/electric_heater.h"
#include "control/gas_heater.h"
#include "core/settings.h"
#include "storage/logger.h"
#include "storage/fs_manager.h"
#include <LittleFS.h>
#include <Update.h>

// WiFiPortal из lib/
#include "WiFiPortal.h"

// ============================================================================
// WiFi Scan состояние (синхронное сканирование — как в рабочем WiFi_Clock_OLED)
// ============================================================================

// ============================================================================
// Singleton
// ============================================================================

WebAPI& WebAPI::getInstance() {
    static WebAPI instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool WebAPI::begin(uint16_t port,
                   DS18B20Manager& sensors,
                   IHeater& heater,
                   PhaseSelector& phase_sel,
                   SettingsManager& settings,
                   Logger& logger) {
    sensors_ = &sensors;
    heater_ = &heater;
    phase_sel_ = &phase_sel;
    settings_ = &settings;
    logger_ = &logger;

    server_ = new AsyncWebServer(port);
    ws_ = new AsyncWebSocket("/ws");

    // WebSocket callback
    ws_->onEvent([](AsyncWebSocket* server, AsyncWebSocketClient* client,
                    AwsEventType type, void* arg, uint8_t* data, size_t len) {
        getInstance().onWsEvent(server, client, type, arg, data, len);
    });
    server_->addHandler(ws_);

    // --- HTTP Endpoints ---

    // GET /api/status/snapshot
    server_->on("/api/status/snapshot", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto& self = getInstance();
        const auto& sm = StateMachine::getInstance();

        JsonDocument doc;
        doc["phase"] = phaseToString(sm.getPhase());
        doc["heater_power"] = self.heater_->getPower();
        doc["heater_enabled"] = self.heater_->isHeatingEnabled();
        doc["heater_type"] = heaterTypeToString(self.heater_->getType());
        doc["delta_t"] = self.phase_sel_->getDeltaT();
        doc["pid_out_cube"] = sm.getPIDCubeOut();
        doc["pid_out_cooler"] = sm.getPIDCoolerOut();
        doc["uptime"] = millis() / 1000;
        doc["calibrating"] = sm.isCalibrating();

        // Датчики
        const SensorData* sd = self.sensors_->getSensors();
        uint8_t count = self.sensors_->getSensorCount();
        JsonArray sensors_arr = doc["sensors"].to<JsonArray>();
        for (uint8_t i = 0; i < count; i++) {
            JsonObject s = sensors_arr.add<JsonObject>();
            s["role"] = roleToString(sd[i].role);
            s["name"] = sd[i].name;
            s["temp"] = sd[i].temp_corrected;
            s["raw"] = sd[i].temp_raw;
            s["present"] = sd[i].present;
            s["address"] = sd[i].address_hex;
            s["offset"] = sd[i].offset;
        }

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // GET /api/log/latest
    server_->on("/api/log/latest", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto& self = getInstance();
        uint16_t n = 50;
        if (request->hasParam("n")) {
            n = request->getParam("n")->value().toInt();
            if (n > 100) n = 100;
        }

        // Используем String — данные копируются и живут пока response не отправлен
        char* tmp = (char*)malloc(4096);
        if (!tmp) {
            request->send(500, "application/json", "{\"error\":\"out_of_memory\"}");
            return;
        }

        self.logger_->getLastEntries(tmp, 4096, n);
        tmp[4095] = '\0';

        String output = String(tmp);
        free(tmp); // String скопировал данные — можно освобождать

        request->send(200, "application/x-ndjson", output);
    });

    // GET /api/config
    server_->on("/api/config", HTTP_GET, [](AsyncWebServerRequest* request) {
        auto& self = getInstance();
        const SystemConfig& cfg = self.settings_->getConfig();

        JsonDocument doc;
        doc["system"]["device_name"] = cfg.device_name;
        doc["system"]["wifi_ssid"] = cfg.wifi_ssid;
        // WiFi пароль НЕ отправляем в открытом виде
        doc["heater"]["type"] = heaterTypeToString(cfg.heater_type);
        doc["heater"]["pin_pwm"] = cfg.pin_pwm;
        doc["heater"]["pin_cooler"] = cfg.pin_cooler;
        doc["phase_thresholds"]["heads_end"] = cfg.threshold_heads_end;
        doc["phase_thresholds"]["body_end"] = cfg.threshold_body_end;
        doc["phase_thresholds"]["delta_hysteresis"] = cfg.delta_hysteresis;
        doc["pid_cube"]["kp"] = cfg.pid_cube.kp;
        doc["pid_cube"]["ki"] = cfg.pid_cube.ki;
        doc["pid_cube"]["kd"] = cfg.pid_cube.kd;
        doc["pid_cube"]["out_min"] = cfg.pid_cube.out_min;
        doc["pid_cube"]["out_max"] = cfg.pid_cube.out_max;
        doc["pid_cube"]["integral_max"] = cfg.pid_cube.integral_max;
        doc["pid_cooler"]["kp"] = cfg.pid_cooler.kp;
        doc["pid_cooler"]["ki"] = cfg.pid_cooler.ki;
        doc["pid_cooler"]["kd"] = cfg.pid_cooler.kd;
        doc["pid_cooler"]["out_min"] = cfg.pid_cooler.out_min;
        doc["pid_cooler"]["out_max"] = cfg.pid_cooler.out_max;
        doc["pid_cooler"]["integral_max"] = cfg.pid_cooler.integral_max;
        doc["mqtt"]["enabled"] = cfg.mqtt_enabled;
        doc["mqtt"]["host"] = cfg.mqtt_host;
        doc["mqtt"]["port"] = cfg.mqtt_port;
        doc["tft"]["enabled"] = cfg.tft_enabled;

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // POST /api/command — принимаем JSON в теле запроса
    server_->on("/api/command", HTTP_POST,
                [](AsyncWebServerRequest* request) {},
                nullptr,
                [this](AsyncWebServerRequest* request, uint8_t* data,
                       size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            request->send(400, "application/json",
                          "{\"error\":\"invalid_json\"}");
            return;
        }
        if (!doc["cmd"].is<const char*>()) {
            request->send(400, "application/json",
                          "{\"error\":\"missing_cmd\"}");
            return;
        }

        getInstance().handleHTTPCommand(request, doc);
    });

    // --- WiFi Setup API ---

    // GET /api/wifi/scan — СИНХРОННОЕ сканирование сетей
    // СКАНИРУЕМ в текущем режиме (AP_STA) — ESP32 умеет сканировать без переключения!
    server_->on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* request) {
        Serial.println("[WiFi] Starting synchronous network scan...");

        // НЕ переключаем режимы! ESP32 может сканировать в AP_STA режиме.
        // Переключение режимов УБИВАЕТ AP и клиенты отваливаются.
        
        // Синхронное сканирование — блокирует до завершения
        int n = WiFi.scanNetworks();
        Serial.printf("[WiFi] scanNetworks() returned: %d\n", n);

        JsonDocument doc;

        if (n == WIFI_SCAN_FAILED) {
            doc["status"] = "error";
            doc["message"] = "Scan failed";
            doc["networks"] = JsonArray();
            Serial.println("[WiFi] Scan failed");
        } else if (n == 0) {
            doc["status"] = "complete";
            doc["message"] = "No networks found";
            JsonArray arr = doc["networks"].to<JsonArray>();
            Serial.println("[WiFi] Scan complete: no networks found");
        } else {
            doc["status"] = "complete";
            doc["message"] = "Scan complete";

            JsonArray networks = doc["networks"].to<JsonArray>();
            int maxNetworks = (n > 20) ? 20 : n;

            for (int i = 0; i < maxNetworks; i++) {
                JsonObject net = networks.add<JsonObject>();
                String ssid = WiFi.SSID(i);
                ssid.replace("\"", "\\\"");
                net["ssid"] = ssid;
                net["rssi"] = WiFi.RSSI(i);
                net["auth"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "wpa";
                net["channel"] = WiFi.channel(i);
            }

            Serial.printf("[WiFi] Scan complete: %d networks found\n", n);
        }

        // Очищаем результаты сканирования
        WiFi.scanDelete();

        // НЕ переключаем режимы! AP продолжает работать.

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // GET /api/wifi/scan-results — alias для совместимости с фронтендом
    server_->on("/api/wifi/scan-results", HTTP_GET, [](AsyncWebServerRequest* request) {
        Serial.println("[WiFi] Sync scan via /api/wifi/scan-results...");
        
        int n = WiFi.scanNetworks();
        Serial.printf("[WiFi] scanNetworks() returned: %d\n", n);

        JsonDocument doc;
        doc["status"] = (n >= 0) ? "complete" : "error";

        if (n > 0) {
            JsonArray networks = doc["networks"].to<JsonArray>();
            int maxNetworks = (n > 20) ? 20 : n;
            for (int i = 0; i < maxNetworks; i++) {
                JsonObject net = networks.add<JsonObject>();
                String ssid = WiFi.SSID(i);
                ssid.replace("\"", "\\\"");
                net["ssid"] = ssid;
                net["rssi"] = WiFi.RSSI(i);
                net["auth"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "wpa";
                net["channel"] = WiFi.channel(i);
            }
        } else {
            doc["networks"] = JsonArray();
        }

        WiFi.scanDelete();

        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });

    // POST /api/wifi/save — сохранение WiFi настроек
    server_->on("/api/wifi/save", HTTP_POST,
                [](AsyncWebServerRequest* request) {},
                nullptr,
                [](AsyncWebServerRequest* request, uint8_t* data,
                       size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            Serial.printf("[WIFI-SAVE] JSON error: %s\n", err.c_str());
            request->send(400, "application/json",
                          "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        const char* ssid = doc["ssid"] | "";
        const char* password = doc["password"] | "";

        Serial.printf("[WIFI-SAVE] ssid='%s' pass_len=%d\n", ssid, strlen(password));

        if (strlen(ssid) == 0) {
            request->send(400, "application/json",
                          "{\"success\":false,\"message\":\"SSID required\"}");
            return;
        }

        // Сохраняем в settings.json
        SystemConfig& cfg = SettingsManager::getInstance().getConfigMutable();
        strncpy(cfg.wifi_ssid, ssid, CFG_MAX_SSID_LEN - 1);
        cfg.wifi_ssid[CFG_MAX_SSID_LEN - 1] = '\0';
        strncpy(cfg.wifi_pass, password, CFG_MAX_WIFI_PASS_LEN - 1);
        cfg.wifi_pass[CFG_MAX_WIFI_PASS_LEN - 1] = '\0';

        bool saved = SettingsManager::getInstance().save();
        Serial.printf("[WIFI-SAVE] save() returned: %s\n", saved ? "true" : "false");

        if (saved) {
            request->send(200, "application/json",
                          "{\"success\":true,\"message\":\"WiFi settings saved. Reboot to apply.\"}");
        } else {
            request->send(500, "application/json",
                          "{\"success\":false,\"message\":\"Failed to save to file\"}");
        }
    });

    // POST /api/mqtt/save — сохранение MQTT настроек
    server_->on("/api/mqtt/save", HTTP_POST,
                [](AsyncWebServerRequest* request) {},
                nullptr,
                [](AsyncWebServerRequest* request, uint8_t* data,
                       size_t len, size_t index, size_t total) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            Serial.printf("[MQTT-SAVE] JSON error: %s\n", err.c_str());
            request->send(400, "application/json",
                          "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        SystemConfig& cfg = SettingsManager::getInstance().getConfigMutable();
        cfg.mqtt_enabled = doc["enabled"] | false;
        
        const char* host = doc["host"] | "";
        strncpy(cfg.mqtt_host, host, MAX_MQTT_HOST_LEN - 1);
        cfg.mqtt_host[MAX_MQTT_HOST_LEN - 1] = '\0';
        
        cfg.mqtt_port = doc["port"] | 1883;
        
        const char* user = doc["user"] | "";
        strncpy(cfg.mqtt_user, user, MAX_CRED_LEN - 1);
        cfg.mqtt_user[MAX_CRED_LEN - 1] = '\0';
        
        const char* pass = doc["pass"] | "";
        strncpy(cfg.mqtt_pass, pass, MAX_CRED_LEN - 1);
        cfg.mqtt_pass[MAX_CRED_LEN - 1] = '\0';
        
        const char* topic = doc["topic"] | "distill";
        strncpy(cfg.mqtt_topic_root, topic, MAX_TOPIC_LEN - 1);
        cfg.mqtt_topic_root[MAX_TOPIC_LEN - 1] = '\0';

        Serial.printf("[MQTT-SAVE] enabled=%d host='%s' port=%d topic='%s'\n",
                      cfg.mqtt_enabled, cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_topic_root);

        bool saved = SettingsManager::getInstance().save();
        if (saved) {
            request->send(200, "application/json",
                          "{\"success\":true,\"message\":\"MQTT settings saved. Reboot to apply.\"}");
        } else {
            request->send(500, "application/json",
                          "{\"success\":false,\"message\":\"Failed to save to file\"}");
        }
    });

    // POST /api/reboot — перезагрузка
    server_->on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Rebooting\"}");
        delay(500);
        ESP.restart();
    });

    // POST /api/ota — OTA обновление прошивки
    server_->on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            if (Update.end(true)) {
                request->send(200, "application/json",
                              "{\"success\":true,\"message\":\"Update complete. Rebooting...\"}");
                delay(500);
                ESP.restart();
            } else {
                request->send(500, "application/json",
                              "{\"success\":false,\"message\":\"Update failed\"}");
            }
        },
        [](AsyncWebServerRequest* request, const String& filename, uint64_t index, uint8_t* data, size_t len, bool final) {
            if (!index) {
                Serial.printf("[OTA] Starting update: %s (size: %u)\n", filename.c_str(), request->contentLength());
                if (!Update.begin(request->contentLength())) {
                    Serial.printf("[OTA] Begin failed: %s\n", Update.errorString());
                    return request->send(400, "application/json",
                                  "{\"success\":false,\"message\":\"Begin failed\"}");
                }
            }
            if (len > 0) {
                if (Update.write(data, len) != len) {
                    Serial.printf("[OTA] Write error: %s\n", Update.errorString());
                    return request->send(500, "application/json",
                                  "{\"success\":false,\"message\":\"Write error\"}");
                }
            }
            if (final) {
                Serial.printf("[OTA] Upload complete, size: %u\n", Update.size());
            }
        });

    // Captive Portal endpoints (Android/iOS/Windows detection)
    // Эти endpoint-ы должны идти ДО onNotFound
    server_->on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* request) {
        // Android captive portal detection — возвращаем 204 No Content
        request->send(204);
    });
    server_->on("/gen_204", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(204);
    });
    server_->on("/fwlink/", HTTP_GET, [](AsyncWebServerRequest* request) {
        // Microsoft captive portal — редирект на главную
        request->redirect("/");
    });
    server_->on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* request) {
        // iOS captive portal — редирект на главную
        request->redirect("/");
    });
    server_->on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest* request) {
        // Windows 10/11 captive portal detection
        request->send(204);
    });
    server_->on("/redirect", HTTP_GET, [](AsyncWebServerRequest* request) {
        // Another Windows endpoint
        request->send(204);
    });

    // Статические файлы SPA из LittleFS (плоская структура)
    // Настраиваем отдачу файлов с проверкой существования
    server_->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // Captive Portal: все неизвестные запросы → setup.html или index.html
    server_->onNotFound([](AsyncWebServerRequest* request) {
        String url = request->url();
        Serial.printf("[Captive] Unknown request: %s\n", url.c_str());
        
        // Проверяем существует ли запрошенный файл через FSManager
        if (FSManager::getInstance().exists(url.c_str())) {
            request->send(LittleFS, url, String());
            return;
        }
        
        // Проверяем setup.html для captive portal
        if (FSManager::getInstance().exists("/setup.html")) {
            Serial.println("[Captive] Redirecting to /setup.html");
            request->redirect("/setup.html");
        } else if (FSManager::getInstance().exists("/index.html")) {
            // Fallback на index.html если setup.html отсутствует
            Serial.println("[Captive] setup.html not found, redirecting to /index.html");
            request->redirect("/index.html");
        } else {
            // Если ничего нет — возвращаем 404
            Serial.println("[Captive] No HTML files found, returning 404");
            request->send(404, "text/plain", "Not Found");
        }
    });

    server_->begin();

    // Добавляем заголовки для отключения кеширования ВСЕХ ответов
    // Это КРИТИЧЕСКИ важно чтобы браузеры всегда получали свежие JS/CSS/HTML
    DefaultHeaders::Instance().addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    DefaultHeaders::Instance().addHeader("Pragma", "no-cache");
    DefaultHeaders::Instance().addHeader("Expires", "0");

    Serial.printf("[WEB] API started on port %d\n", port);
    return true;
}

// ============================================================================
// handleHTTPCommand
// ============================================================================

void WebAPI::handleHTTPCommand(AsyncWebServerRequest* request, JsonDocument& doc) {
    const char* cmd = doc["cmd"];

    if (strcmp(cmd, "emergency_stop") == 0) {
        StateMachine::getInstance().emergencyStop();
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Emergency stop\"}");
        return;
    }

    if (strcmp(cmd, "reset_error") == 0) {
        StateMachine::getInstance().resetError();
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Error reset\"}");
        return;
    }

    if (strcmp(cmd, "calibrate") == 0) {
        StateMachine::getInstance().startCalibration();
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Calibration started\"}");
        return;
    }

    if (strcmp(cmd, "scan_sensors") == 0) {
        JsonDocument doc_response;
        doc_response["type"] = "sensors";
        JsonArray arr = doc_response["sensors"].to<JsonArray>();
        
        const SensorData* s_array = sensors_->getSensors();
        uint8_t count = sensors_->getSensorCount();
        
        // Сохраняем найденные датчики в конфиг
        if (count > 0) {
            auto& cfg = settings_->getConfigMutable();
            cfg.sensor_count = count;
            for (uint8_t i = 0; i < count && i < MAX_SENSORS; i++) {
                cfg.sensors[i].role = s_array[i].role;
                strncpy(cfg.sensors[i].address_hex, s_array[i].address_hex, sizeof(cfg.sensors[i].address_hex) - 1);
                strncpy(cfg.sensors[i].name, s_array[i].name, MAX_NAME_LEN - 1);
                cfg.sensors[i].active = true; // По умолчанию активирован
                cfg.sensors[i].calibrate = false;
                cfg.sensors[i].offset = 0;
                cfg.sensors[i].manual_offset = 0;
            }
            settings_->save();
            Serial.printf("[API] Saved %d sensors to config\n", count);
        }
        
        for (uint8_t i = 0; i < count && i < 4; i++) {
            JsonObject s = arr.add<JsonObject>();
            s["address"] = s_array[i].address_hex;
            s["present"] = s_array[i].present;
            s["temp"] = s_array[i].temp_corrected;
            s["role"] = roleToString(s_array[i].role);
            s["active"] = s_array[i].active;
            s["name"] = s_array[i].name;
        }
        
        String output;
        serializeJson(doc_response, output);
        request->send(200, "application/json", output);
        return;
    }

    if (strcmp(cmd, "reboot") == 0) {
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Rebooting...\"}");
        delay(100);
        ESP.restart();
        return;
    }

    if (strcmp(cmd, "set_power") == 0) {
        uint8_t power = doc["value"] | 0;
        if (heater_->getType() == HeaterType::ELECTRIC) {
            auto& eh = static_cast<ElectricHeater&>(*heater_);
            eh.setManualPower(power);
        } else {
            heater_->setPower(power);
        }
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Power set\"}");
        return;
    }

    if (strcmp(cmd, "set_phase") == 0) {
        const char* p = doc["phase"] | "idle";
        StateMachine::getInstance().setPhase(stringToPhase(p));
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Phase set\"}");
        return;
    }

    if (strcmp(cmd, "start") == 0) {
        StateMachine::getInstance().start();
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Process started\"}");
        return;
    }

    if (strcmp(cmd, "stop") == 0) {
        StateMachine::getInstance().stop();
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Process stopped\"}");
        return;
    }

    // --- v1.1: Новые команды ---

    if (strcmp(cmd, "set_mode") == 0) {
        const char* m = doc["mode"] | "rectification";
        OperationMode mode = stringToMode(m);
        StateMachine::getInstance().setMode(mode);
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Mode set\"}");
        return;
    }

    if (strcmp(cmd, "set_sub_mode") == 0) {
        const char* sm = doc["sub_mode"] | "vapor";
        RectSubMode sub_mode = stringToSubMode(sm);
        StateMachine::getInstance().setRectSubMode(sub_mode);
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Sub-mode set\"}");
        return;
    }

    if (strcmp(cmd, "set_cooler_type") == 0) {
        const char* ct = doc["cooler_type"] | "fan";
        CoolerType type = stringToCoolerType(ct);
        heater_->setCoolerType(type);
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Cooler type set\"}");
        return;
    }

    if (strcmp(cmd, "set_gas_control_mode") == 0) {
        const char* gm = doc["gas_control_mode"] | "auto";
        GasControlMode gmode = stringToGasControlMode(gm);
        if (heater_->getType() == HeaterType::GAS) {
            auto& gh = static_cast<GasHeater&>(*heater_);
            gh.setControlMode(gmode);
        }
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Gas control mode set\"}");
        return;
    }

    if (strcmp(cmd, "set_distillation_params") == 0) {
        float target = doc["target_temp"] | 78.0f;
        float tolerance = doc["temp_tolerance"] | 0.5f;
        auto& cfg = settings_->getConfigMutable();
        cfg.dist_target_temp = target;
        cfg.dist_temp_tolerance = tolerance;
        settings_->save();
        request->send(200, "application/json",
                      "{\"success\":true,\"message\":\"Distillation params set\"}");
        return;
    }

    if (strcmp(cmd, "set_sensor_role") == 0) {
        const char* addr = doc["address"] | "";
        const char* role = doc["role"] | "cooler";
        bool found = false;
        auto& cfg = settings_->getConfigMutable();
        for (uint8_t i = 0; i < cfg.sensor_count; i++) {
            if (strcmp(cfg.sensors[i].address_hex, addr) == 0) {
                cfg.sensors[i].role = stringToRole(role);
                found = true;
                break;
            }
        }
        if (found) {
            settings_->save();
            request->send(200, "application/json",
                          "{\"success\":true,\"message\":\"Sensor role updated\"}");
        } else {
            request->send(404, "application/json",
                          "{\"error\":\"sensor_not_found\"}");
        }
        return;
    }

    // set_sensor - сохранение настроек датчика
    if (strcmp(cmd, "set_sensor") == 0) {
        uint8_t idx = 255;
        if (doc.containsKey("idx")) {
            idx = doc["idx"].as<uint8_t>();
        }
        
        Serial.printf("[API] set_sensor: idx=%d\n", idx);
        
        if (idx < MAX_SENSORS) {
            auto& cfg = settings_->getConfigMutable();
            
            if (idx < cfg.sensor_count) {
                if (doc.containsKey("role")) {
                    cfg.sensors[idx].role = stringToRole(doc["role"]);
                }
                if (doc.containsKey("name")) {
                    strncpy(cfg.sensors[idx].name, doc["name"].as<const char*>(), MAX_NAME_LEN - 1);
                }
                if (doc.containsKey("offset")) {
                    cfg.sensors[idx].manual_offset = doc["offset"].as<float>();
                }
                if (doc.containsKey("calibrate")) {
                    cfg.sensors[idx].calibrate = doc["calibrate"].as<bool>();
                }
                if (doc.containsKey("active")) {
                    cfg.sensors[idx].active = doc["active"].as<bool>();
                }
                
                settings_->save();
                request->send(200, "application/json",
                              "{\"success\":true,\"message\":\"Sensor updated\"}");
            } else {
                request->send(404, "application/json",
                              "{\"error\":\"sensor_idx_invalid\"}");
            }
        } else {
            request->send(400, "application/json",
                          "{\"error\":\"invalid_idx\"}");
        }
        return;
    }

    if (strcmp(cmd, "set_sensor_active") == 0) {
        const char* addr = doc["address"] | "";
        bool active = doc["active"] | true;
        bool found = false;
        auto& cfg = settings_->getConfigMutable();
        for (uint8_t i = 0; i < cfg.sensor_count; i++) {
            if (strcmp(cfg.sensors[i].address_hex, addr) == 0) {
                cfg.sensors[i].active = active;
                found = true;
                break;
            }
        }
        if (found) {
            settings_->save();
            request->send(200, "application/json",
                          "{\"success\":true,\"message\":\"Sensor active updated\"}");
        } else {
            request->send(404, "application/json",
                          "{\"error\":\"sensor_not_found\"}");
        }
        return;
    }

    request->send(400, "application/json",
                  "{\"error\":\"unknown_command\"}");
}

// ============================================================================
// broadcastTelemetry
// ============================================================================

void WebAPI::broadcastTelemetry(const SystemSnapshot& snapshot) {
    if (!ws_ || ws_->count() == 0) return;

    JsonDocument doc;
    buildTelemetryJSON(snapshot, doc);

    String output;
    serializeJson(doc, output);
    ws_->textAll(output);
}

// ============================================================================
// poll
// ============================================================================

void WebAPI::poll() {
    ws_->cleanupClients();
}

// ============================================================================
// getClientCount
// ============================================================================

uint32_t WebAPI::getClientCount() const {
    return ws_ ? ws_->count() : 0;
}

// ============================================================================
// onWsEvent
// ============================================================================

void WebAPI::onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("[WS] Client connected: %u (total: %u)\n",
                      client->id(), server->count());
        return;
    }

    if (type == WS_EVT_DISCONNECT) {
        Serial.printf("[WS] Client disconnected: %u (total: %u)\n",
                      client->id(), server->count());
        return;
    }

    if (type == WS_EVT_DATA) {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->opcode == WS_TEXT && info->final) {
            data[len] = '\0';
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, (char*)data, len);
            if (err) {
                getInstance().sendWsResponse(client, false, "Invalid JSON");
                return;
            }
            getInstance().handleWsCommand(client, doc);
        }
    }
}

// ============================================================================
// handleWsCommand
// ============================================================================

void WebAPI::handleWsCommand(AsyncWebSocketClient* client, JsonDocument& doc) {
    if (!doc.containsKey("cmd")) {
        sendWsResponse(client, false, "Missing 'cmd' field");
        return;
    }

    const char* cmd = doc["cmd"];

    if (strcmp(cmd, "emergency_stop") == 0) {
        StateMachine::getInstance().emergencyStop();
        sendWsResponse(client, true, "Emergency stop");
        return;
    }

    if (strcmp(cmd, "reset_error") == 0) {
        StateMachine::getInstance().resetError();
        sendWsResponse(client, true, "Error reset");
        return;
    }

    if (strcmp(cmd, "calibrate") == 0) {
        StateMachine::getInstance().startCalibration();
        sendWsResponse(client, true, "Calibration started");
        return;
    }

    if (strcmp(cmd, "scan_sensors") == 0) {
        JsonDocument doc_response;
        doc_response["type"] = "sensors";
        JsonArray arr = doc_response["sensors"].to<JsonArray>();
        
        const SensorData* s_array = sensors_->getSensors();
        uint8_t count = sensors_->getSensorCount();
        
        // Сохраняем найденные датчики в конфиг
        if (count > 0) {
            auto& cfg = settings_->getConfigMutable();
            cfg.sensor_count = count;
            for (uint8_t i = 0; i < count && i < MAX_SENSORS; i++) {
                cfg.sensors[i].role = s_array[i].role;
                strncpy(cfg.sensors[i].address_hex, s_array[i].address_hex, sizeof(cfg.sensors[i].address_hex) - 1);
                strncpy(cfg.sensors[i].name, s_array[i].name, MAX_NAME_LEN - 1);
                cfg.sensors[i].active = s_array[i].active;
                cfg.sensors[i].calibrate = false;
                cfg.sensors[i].offset = 0;
                cfg.sensors[i].manual_offset = 0;
            }
            settings_->save();
            Serial.printf("[WS] Saved %d sensors to config\n", count);
        }
        
        for (uint8_t i = 0; i < count && i < 4; i++) {
            JsonObject s = arr.add<JsonObject>();
            s["address"] = s_array[i].address_hex;
            s["present"] = s_array[i].present;
            s["temp"] = s_array[i].temp_corrected;
            s["role"] = roleToString(s_array[i].role);
            s["active"] = s_array[i].active;
            s["name"] = s_array[i].name;
        }
        
        String output;
        serializeJson(doc_response, output);
        client->text(output);
        return;
    }

    if (strcmp(cmd, "reboot") == 0) {
        sendWsResponse(client, true, "Rebooting...");
        delay(100);
        ESP.restart();
        return;
    }

    if (strcmp(cmd, "set_power") == 0) {
        uint8_t power = doc["value"] | 0;
        if (heater_->getType() == HeaterType::ELECTRIC) {
            auto& eh = static_cast<ElectricHeater&>(*heater_);
            eh.setManualPower(power);
        } else {
            heater_->setPower(power);
        }
        sendWsResponse(client, true, "Power set");
        return;
    }

    if (strcmp(cmd, "set_pid") == 0) {
        const char* target = doc["target"] | "";
        PIDParams params;
        params.kp = doc["kp"] | 0.0f;
        params.ki = doc["ki"] | 0.0f;
        params.kd = doc["kd"] | 0.0f;
        params.out_min = doc["out_min"] | 0.0f;
        params.out_max = doc["out_max"] | 100.0f;
        params.integral_max = doc["integral_max"] | 50.0f;

        bool ok = false;
        if (strcmp(target, "cube") == 0 || strcmp(target, "heater") == 0) {
            heater_->setHeaterPIDParams(params);
            settings_->savePIDCubeParams(params);
            ok = true;
        } else if (strcmp(target, "cooler") == 0) {
            heater_->setCoolerPIDParams(params);
            settings_->savePIDCoolerParams(params);
            ok = true;
        } else if (strcmp(target, "dist_cooler") == 0) {
            settings_->savePIDDistCoolerParams(params);
            ok = true;
        }
        sendWsResponse(client, ok, ok ? "PID updated" : "Invalid target");
        return;
    }

    if (strcmp(cmd, "set_threshold") == 0) {
        float heads = doc["heads_end"] | 1.2f;
        float body = doc["body_end"] | 0.3f;
        float hys = doc["hysteresis"] | 0.1f;
        settings_->savePhaseThresholds(heads, body, hys);
        phase_sel_->refreshThresholds();
        sendWsResponse(client, true, "Thresholds updated");
        return;
    }

    if (strcmp(cmd, "set_phase") == 0) {
        const char* p = doc["phase"] | "idle";
        StateMachine::getInstance().setPhase(stringToPhase(p));
        sendWsResponse(client, true, "Phase set");
        return;
    }

    if (strcmp(cmd, "start") == 0) {
        StateMachine::getInstance().start();
        sendWsResponse(client, true, "Process started");
        return;
    }

    if (strcmp(cmd, "stop") == 0) {
        StateMachine::getInstance().stop();
        sendWsResponse(client, true, "Process stopped");
        return;
    }

    // --- v1.1: Новые команды ---

    if (strcmp(cmd, "set_mode") == 0) {
        const char* m = doc["mode"] | "rectification";
        OperationMode mode = stringToMode(m);
        StateMachine::getInstance().setMode(mode);
        sendWsResponse(client, true, "Mode set");
        return;
    }

    if (strcmp(cmd, "set_sub_mode") == 0) {
        const char* sm = doc["sub_mode"] | "vapor";
        RectSubMode sub_mode = stringToSubMode(sm);
        StateMachine::getInstance().setRectSubMode(sub_mode);
        sendWsResponse(client, true, "Sub-mode set");
        return;
    }

    if (strcmp(cmd, "set_cooler_type") == 0) {
        const char* ct = doc["cooler_type"] | "fan";
        CoolerType type = stringToCoolerType(ct);
        heater_->setCoolerType(type);
        sendWsResponse(client, true, "Cooler type set");
        return;
    }

    if (strcmp(cmd, "set_gas_control_mode") == 0) {
        const char* gm = doc["gas_control_mode"] | "auto";
        GasControlMode gmode = stringToGasControlMode(gm);
        if (heater_->getType() == HeaterType::GAS) {
            auto& gh = static_cast<GasHeater&>(*heater_);
            gh.setControlMode(gmode);
        }
        sendWsResponse(client, true, "Gas control mode set");
        return;
    }

    if (strcmp(cmd, "set_distillation_params") == 0) {
        float target = doc["target_temp"] | 78.0f;
        float tolerance = doc["temp_tolerance"] | 0.5f;
        auto& cfg = settings_->getConfigMutable();
        cfg.dist_target_temp = target;
        cfg.dist_temp_tolerance = tolerance;
        settings_->save();
        sendWsResponse(client, true, "Distillation params set");
        return;
    }

    if (strcmp(cmd, "set_sensor_role") == 0) {
        const char* addr = doc["address"] | "";
        const char* role = doc["role"] | "cooler";
        bool found = false;
        auto& cfg = settings_->getConfigMutable();
        for (uint8_t i = 0; i < cfg.sensor_count; i++) {
            if (strcmp(cfg.sensors[i].address_hex, addr) == 0) {
                cfg.sensors[i].role = stringToRole(role);
                found = true;
                break;
            }
        }
        if (found) {
            settings_->save();
            sendWsResponse(client, true, "Sensor role updated");
        } else {
            sendWsResponse(client, false, "Sensor not found");
        }
        return;
    }

    if (strcmp(cmd, "set_sensor_active") == 0) {
        const char* addr = doc["address"] | "";
        bool active = doc["active"] | true;
        bool found = false;
        auto& cfg = settings_->getConfigMutable();
        for (uint8_t i = 0; i < cfg.sensor_count; i++) {
            if (strcmp(cfg.sensors[i].address_hex, addr) == 0) {
                cfg.sensors[i].active = active;
                found = true;
                break;
            }
        }
        if (found) {
            settings_->save();
            sendWsResponse(client, true, "Sensor active updated");
        } else {
            sendWsResponse(client, false, "Sensor not found");
        }
        return;
    }

    // set_sensor - сохранение настроек датчика
    if (strcmp(cmd, "set_sensor") == 0) {
        uint8_t idx = 255;
        if (doc.containsKey("idx")) {
            idx = doc["idx"].as<uint8_t>();
        }
        
        Serial.printf("[WS] set_sensor: idx=%d\n", idx);
        
        if (idx < MAX_SENSORS) {
            auto& cfg = settings_->getConfigMutable();
            
            if (idx < cfg.sensor_count) {
                if (doc.containsKey("role")) {
                    cfg.sensors[idx].role = stringToRole(doc["role"]);
                }
                if (doc.containsKey("name")) {
                    strncpy(cfg.sensors[idx].name, doc["name"].as<const char*>(), MAX_NAME_LEN - 1);
                }
                if (doc.containsKey("offset")) {
                    cfg.sensors[idx].manual_offset = doc["offset"].as<float>();
                }
                if (doc.containsKey("calibrate")) {
                    cfg.sensors[idx].calibrate = doc["calibrate"].as<bool>();
                }
                if (doc.containsKey("active")) {
                    cfg.sensors[idx].active = doc["active"].as<bool>();
                }
                
                settings_->save();
                sendWsResponse(client, true, "Sensor updated");
            } else {
                sendWsResponse(client, false, "Invalid sensor index");
            }
        } else {
            sendWsResponse(client, false, "Invalid idx");
        }
        return;
    }

    sendWsResponse(client, false, "Unknown command");
}

// ============================================================================
// buildTelemetryJSON
// ============================================================================

void WebAPI::buildTelemetryJSON(const SystemSnapshot& snapshot, JsonDocument& doc) {
    doc["type"] = "telemetry";
    doc["mode"] = modeToString(snapshot.mode);
    doc["phase"] = phaseToString(snapshot.phase);
    doc["delta_t"] = snapshot.delta_t;
    doc["heater_power"] = snapshot.heater_power;
    doc["pid_out_cube"] = snapshot.pid_out_cube;
    doc["pid_out_cooler"] = snapshot.pid_out_cooler;
    doc["heater_type"] = heaterTypeToString(snapshot.heater_type);
    doc["heater_enabled"] = snapshot.heater_enabled;
    doc["errors"] = snapshot.error_count;
    doc["calibrating"] = snapshot.calibrating;
    doc["uptime"] = snapshot.uptime_sec;
    doc["ts"] = snapshot.timestamp_ms;

    // Distillation-specific
    if (snapshot.mode == OperationMode::DISTILLATION) {
        doc["dist_target_temp"] = snapshot.dist_target_temp;
        doc["dist_current_temp"] = snapshot.dist_current_temp;
    }

    JsonArray temps = doc["temps"].to<JsonArray>();
    for (uint8_t i = 0; i < snapshot.sensor_count; i++) {
        JsonObject t = temps.add<JsonObject>();
        t["role"] = roleToString(snapshot.sensors[i].role);
        t["name"] = snapshot.sensors[i].name;
        t["temp"] = snapshot.sensors[i].temp_corrected;
        t["raw"] = snapshot.sensors[i].temp_raw;
        t["present"] = snapshot.sensors[i].present;
        t["offset"] = snapshot.sensors[i].offset;
        t["active"] = snapshot.sensors[i].active;
    }
}

// ============================================================================
// sendWsResponse
// ============================================================================

void WebAPI::sendWsResponse(AsyncWebSocketClient* client, bool success, const char* message) {
    if (!client) return;

    JsonDocument doc;
    doc["success"] = success;
    doc["message"] = message;

    String output;
    serializeJson(doc, output);
    client->text(output);
}
