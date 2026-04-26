/**
 * @file mqtt_handler.h
 * @brief MQTT клиент + Home Assistant MQTT Discovery.
 *
 * Функции:
 * 1. Подключение к MQTT-брокеру (настройки из settings.json)
 * 2. Home Assistant Discovery — автоматическое обнаружение устройств
 * 3. Публикация телеметрии (сенсоры, фаза, мощность)
 * 4. Подписка на команды (set_power, emergency_stop, и т.д.)
 * 5. Auto-reconnect при потере соединения
 *
 * Инициализация ТОЛЬКО если mqtt.enabled == true в settings.json.
 * Иначе модуль не стартует, нагрузка на цикл = 0.
 */

#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include "core/config.h"
#include "core/settings.h"
#include <PubSubClient.h>
#include <WiFi.h>

// Forward declarations
class DS18B20Manager;
class IHeater;

/**
 * @brief Класс MQTT обработчика.
 *
 * Singleton. Не блокирует loop — poll() вызывает client.loop().
 * Auto-reconnect с интервалом 5 секунд.
 */
class MQTTHandler {
public:
    /** @brief Получить экземпляр (singleton) */
    static MQTTHandler& getInstance();

    /**
     * @brief Инициализация MQTT.
     * @param settings Ссылка на настройки
     * @param sensors  Ссылка на менеджер датчиков
     * @param heater   Ссылка на нагреватель
     * @return true при успехе (или false если mqtt отключён)
     */
    bool begin(SettingsManager& settings, DS18B20Manager& sensors, IHeater& heater);

    /**
     * @brief Публикация снапшота телеметрии.
     * @param snapshot Текущий снапшот
     */
    void publishSnapshot(const SystemSnapshot& snapshot);

    /**
     * @brief Неблокирующий опрос.
     * Вызывать из loop(): client.loop + auto-reconnect.
     */
    void poll();

    /** @brief Подключён ли к брокеру */
    bool isConnected() const;

    /** @brief MQTT включён в настройках */
    bool isEnabled() const { return enabled_; }

private:
    MQTTHandler() = default;
    MQTTHandler(const MQTTHandler&) = delete;
    MQTTHandler& operator=(const MQTTHandler&) = delete;

    /** Подключиться к брокеру */
    bool connect();

    /** Опубликовать Home Assistant Discovery сообщения */
    void publishDiscovery();

    /** Опубликовать один Discovery конфиг */
    void publishDiscoveryConfig(const char* topic, const char* payload);

    /** Callback при получении MQTT-сообщения */
    static void onMqttMessage(char* topic, byte* payload, unsigned int length);

    /** Обработать входящую команду */
    void handleCommand(const char* topic, const char* payload, size_t length);

    PubSubClient*      client_ = nullptr;
    SettingsManager*   settings_ = nullptr;
    DS18B20Manager*    sensors_ = nullptr;
    IHeater*           heater_ = nullptr;

    char topic_root_[MAX_TOPIC_LEN] = "distill";
    bool enabled_ = false;
    bool connected_ = false;
    bool discovery_sent_ = false;

    uint32_t reconnect_timer_ = 0;

    static constexpr uint32_t RECONNECT_INTERVAL_MS = 5000;
    static constexpr uint32_t PUBLISH_INTERVAL_MS   = 5000;
};

#endif // MQTT_HANDLER_H
