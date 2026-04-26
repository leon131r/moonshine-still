/**
 * @file web_api.h
 * @brief HTTP API + WebSocket (двусторонний).
 *
 * HTTP Endpoints:
 *   GET  /api/status/snapshot  → полное состояние системы (JSON)
 *   GET  /api/log/latest       → последние N записей логов (NDJSON)
 *   GET  /api/config           → текущие настройки (JSON)
 *   POST /api/command          → команды: set_parameter, start_phase,
 *                                emergency_stop, reset_error, calibrate
 *   GET  /                     → SPA из LittleFS (index.html)
 *   GET  /css/*, /js/*         → статические ассеты
 *
 * WebSocket (/ws):
 *   - Сервер → Клиент: push телеметрии каждые 500 мс
 *     {"type":"telemetry","phase":"body","T_top":78.2,...}
 *   - Клиент → Сервер: команды
 *     {"cmd":"set_power","value":50}
 *     {"cmd":"set_pid","target":"cube","kp":2.0,"ki":0.5,"kd":1.0}
 *     {"cmd":"emergency_stop"}
 *     {"cmd":"reset_error"}
 *     {"cmd":"calibrate"}
 *     {"cmd":"set_threshold","heads_end":1.2,"body_end":0.3,"hysteresis":0.1}
 *
 * Все данные в строгом JSON.
 * Ошибки возвращаются с HTTP-кодами (400, 500) и JSON-телом:
 *   {"error":"invalid_command","message":"..."}
 */

#ifndef WEB_API_H
#define WEB_API_H

#include "core/config.h"
#include "core/settings.h"
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>

// Forward declarations
class DS18B20Manager;
class IHeater;
class PhaseSelector;
class Logger;

/**
 * @brief Класс веб-API и WebSocket.
 *
 * Singleton. Асинхронный сервер на ESPAsyncWebServer.
 * Не блокирует loop — все callbacks асинхронные.
 */
class WebAPI {
public:
    /** @brief Получить экземпляр (singleton) */
    static WebAPI& getInstance();

    /**
     * @brief Инициализация.
     * @param port       HTTP порт (обычно 80)
     * @param sensors    Ссылка на менеджер датчиков
     * @param heater     Ссылка на нагреватель
     * @param phase_sel  Ссылка на селектор фаз
     * @param settings   Ссылка на настройки
     * @param logger     Ссылка на логгер
     * @return true при успехе
     */
    bool begin(uint16_t port,
               DS18B20Manager& sensors,
               IHeater& heater,
               PhaseSelector& phase_sel,
               SettingsManager& settings,
               Logger& logger);

    /**
     * @brief Отправить телеметрию всем подключённым клиентам.
     * @param snapshot Текущий снапшот системы
     *
     * Вызывать из loop по таймеру (WS_TELEMETRY_MS).
     * Форматирует snapshot в JSON и отправляет через WebSocket.
     */
    void broadcastTelemetry(const SystemSnapshot& snapshot);

    /**
     * @brief Обработать входящие WebSocket-команды.
     *
     * Вызывать из loop.
     * Команды приходят асинхронно через onEvent callback.
     */
    void poll();

    /** @brief Количество подключённых WebSocket-клиентов */
    uint32_t getClientCount() const;

private:
    WebAPI() = default;
    WebAPI(const WebAPI&) = delete;
    WebAPI& operator=(const WebAPI&) = delete;

    // Ссылки на модули (не владеет)
    DS18B20Manager* sensors_ = nullptr;
    IHeater*        heater_ = nullptr;
    PhaseSelector*  phase_sel_ = nullptr;
    SettingsManager* settings_ = nullptr;
    Logger*         logger_ = nullptr;

    AsyncWebServer* server_ = nullptr;
    AsyncWebSocket* ws_ = nullptr;

    /**
     * @brief Обработчик WebSocket-событий.
     * @param server Указатель на сервер
     * @param client Указатель на клиента
     * @param type   Тип события (connect, disconnect, data, error)
     * @param data   Данные (для data — текст сообщения)
     */
    static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                          AwsEventType type, void* arg, uint8_t* data, size_t len);

    /**
     * @brief Обработать команду от клиента.
     * @param client Указатель на клиента (для ответа)
     * @param doc    Распарсенный JSON-документ команды
     */
    void handleWsCommand(AsyncWebSocketClient* client, JsonDocument& doc);

    /**
     * @brief Сформировать JSON телеметрии.
     * @param snapshot Снапшот
     * @param doc      Документ для заполнения
     */
    void buildTelemetryJSON(const SystemSnapshot& snapshot, JsonDocument& doc);

    /**
     * @brief Отправить ответ клиенту.
     * @param client Клиент
     * @param success true = успех, false = ошибка
     * @param message Текстовое сообщение
     */
    void sendWsResponse(AsyncWebSocketClient* client, bool success, const char* message);

    /**
     * @brief Обработать HTTP POST команду.
     */
    void handleHTTPCommand(AsyncWebServerRequest* request, JsonDocument& doc);
};

#endif // WEB_API_H
