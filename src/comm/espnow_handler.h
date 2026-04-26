/**
 * @file espnow_handler.h
 * @brief Обработчик ESP-NOW для связи с газовым блоком.
 *
 * ESP-NOW — протокол ESP32 для прямой передачи данных без WiFi.
 * Используется для управления газовым нагревателем.
 *
 * Роли:
 * - Контроллер (этот модуль): отправляет GasControlData
 * - Газовый блок: отвечает GasStatusData
 *
 * Инициализация:
 * 1. esp_now_init()
 * 2. esp_now_add_peer(mac, role, channel, key, key_len)
 * 3. Регистрация callback приёма (onReceive)
 *
 * Callback приёма сохраняет последний GasStatusData
 * для чтения через getLastStatus().
 */

#ifndef ESPNOW_HANDLER_H
#define ESPNOW_HANDLER_H

#include "core/config.h"
#include <esp_now.h>
#include <WiFi.h>

/**
 * @brief Класс управления ESP-NOW.
 *
 * Поддерживает 2 пира:
 * - Gas Block (канал 1): отправка управления нагревом
 * - Level Module (канал 2): приём данных об уровне/крепости
 *
 * Singleton. Вызывается из GasHeater для отправки команд.
 * Приём обрабатывается асинхронно через callback.
 */
class ESPNowHandler {
public:
    /** @brief Получить экземпляр (singleton) */
    static ESPNowHandler& getInstance();

    /**
     * @brief Инициализация ESP-NOW с двумя пирами.
     * @param gas_mac     MAC-адрес газового блока (6 байт)
     * @param level_mac  MAC-адрес Level модуля (6 байт)
     * @return true при успехе
     *
     * Последовательность:
     * 1. WiFi.mode(WIFI_STA) — ESP-NOW требует STA или AP+STA
     * 2. esp_now_init()
     * 3. esp_now_add_peer() для газового блока
     * 4. esp_now_add_peer() для Level модуля
     * 5. esp_now_register_recv_cb() — callback приёма
     */
    bool begin(const uint8_t gas_mac[6], const uint8_t level_mac[6]);

    /**
     * @brief Отправить GasControlData на газовый блок.
     * @param data Указатель на данные для отправки
     * @return ESP_OK при успехе
     */
    esp_err_t sendGasControl(const GasControlData* data);

    /**
     * @brief Отправить PID-конфиг на газовый блок (remote PID mode).
     * @param config Указатель на GasPIDConfig
     * @return ESP_OK при успехе
     */
    esp_err_t sendGasPIDConfig(const GasPIDConfig* config);

    /**
     * @brief Получить последний ответ от газового блока.
     * @param out_buf Буфер для GasStatusData
     * @return true если есть свежие данные
     */
    bool getGasStatus(GasStatusData* out_buf);

    /**
     * @brief Получить данные от Level модуля.
     * @param out_buf Буфер для LevelModuleData
     * @return true если есть свежие данные
     */
    bool getLevelData(LevelModuleData* out_buf);

    /** @brief Статус последней отправки (ESP_OK = успех) */
    esp_err_t getLastSendStatus() const { return last_send_status_; }

    /** @brief ESP-NOW инициализирован */
    bool isInitialized() const { return initialized_; }

    /** @brief Level модуль подключён и активен */
    bool isLevelModuleActive() const { return level_peer_added_; }

private:
    ESPNowHandler() = default;
    ESPNowHandler(const ESPNowHandler&) = delete;
    ESPNowHandler& operator=(const ESPNowHandler&) = delete;

    /**
     * @brief Callback приёма данных (статический, для ESP-NOW).
     * @param mac MAC отправителя
     * @param data Указатель на данные
     * @param len  Размер данных
     *
     * Вызывается из ISR-контекста! Только запись в volatile.
     */
    static void onReceive(const uint8_t* mac, const uint8_t* data, int len);

    /**
     * @brief Callback отправки (статический, для ESP-NOW).
     * @param mac MAC получателя
     * @param status Статус (ESP_OK = успех)
     */
    static void onSent(const uint8_t* mac, esp_now_send_status_t status);

    uint8_t            gas_peer_mac_[6] = {0};   ///< MAC газового блока
    uint8_t            level_peer_mac_[6] = {0};  ///< MAC Level модуля
    volatile bool      initialized_ = false;
    volatile bool      gas_peer_added_ = false;
    volatile bool      level_peer_added_ = false;
    volatile bool      has_gas_data_ = false;
    volatile bool      has_level_data_ = false;
    GasStatusData     last_gas_status_;     ///< Последний ответ от газа
    LevelModuleData  last_level_data_;     ///< Последние данные от Level модуля
    volatile esp_err_t last_send_status_ = ESP_FAIL;
};

#endif // ESPNOW_HANDLER_H
