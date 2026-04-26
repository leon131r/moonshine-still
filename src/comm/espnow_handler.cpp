/**
 * @file espnow_handler.cpp
 * @brief Реализация ESP-NOW обработчика с двумя пирами.
 *
 * Канал 1: Gas Block (управление нагревом)
 * Канал 2: Level Module (уровень, ABV, давление)
 *
 * Важные ограничения:
 * - Callback onReceive вызывается из ISR-контекста (прерывание WiFi)
 *   В ISR нельзя использовать Serial.printf, malloc, delay.
 *   Только memcpy в заранее выделенный буфер + установка флага.
 * - Размер данных ESP-NOW: до 250 байт
 * - Канал WiFi должен совпадать у обоих устройств
 */

#include "espnow_handler.h"
#include <Arduino.h>
#include <esp_wifi.h>

// ============================================================================
// Singleton
// ============================================================================

ESPNowHandler& ESPNowHandler::getInstance() {
    static ESPNowHandler instance;
    return instance;
}

// ============================================================================
// begin — инициализация с двумя пирами
// ============================================================================

bool ESPNowHandler::begin(const uint8_t gas_mac[6], const uint8_t level_mac[6]) {
    if (initialized_) {
        return true;
    }

    memcpy(gas_peer_mac_, gas_mac, 6);
    memcpy(level_peer_mac_, level_mac, 6);

    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
        Serial.println("[ESPNOW] WiFi STA enabled");
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] ERROR: esp_now_init failed");
        return false;
    }

    esp_now_register_recv_cb(ESPNowHandler::onReceive);
    esp_now_register_send_cb(ESPNowHandler::onSent);

    // === Gas Block peer ===
    esp_now_peer_info_t gasPeer;
    memset(&gasPeer, 0, sizeof(gasPeer));
    memcpy(gasPeer.peer_addr, gas_peer_mac_, 6);
    gasPeer.channel = 0;
    gasPeer.ifidx = WIFI_IF_STA;
    gasPeer.encrypt = false;

    if (esp_now_add_peer(&gasPeer) != ESP_OK) {
        Serial.println("[ESPNOW] ERROR: Failed to add gas peer");
    } else {
        gas_peer_added_ = true;
    }

    // === Level Module peer ===
    esp_now_peer_info_t levelPeer;
    memset(&levelPeer, 0, sizeof(levelPeer));
    memcpy(levelPeer.peer_addr, level_peer_mac_, 6);
    levelPeer.channel = 0;
    levelPeer.ifidx = WIFI_IF_STA;
    levelPeer.encrypt = false;

    if (esp_now_add_peer(&levelPeer) != ESP_OK) {
        Serial.println("[ESPNOW] WARN: Failed to add level peer (optional)");
    } else {
        level_peer_added_ = true;
    }

    initialized_ = true;

    Serial.printf("[ESPNOW] Initialized: gas=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  gas_mac[0], gas_mac[1], gas_mac[2],
                  gas_mac[3], gas_mac[4], gas_mac[5]);
    if (level_peer_added_) {
        Serial.printf("[ESPNOW] Level module: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      level_mac[0], level_mac[1], level_mac[2],
                      level_mac[3], level_mac[4], level_mac[5]);
    }
    return true;
}

// ============================================================================
// sendGasControl — отправка на Gas Block
// ============================================================================

esp_err_t ESPNowHandler::sendGasControl(const GasControlData* data) {
    if (!initialized_ || !gas_peer_added_) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = esp_now_send(
        gas_peer_mac_,
        (const uint8_t*)data,
        sizeof(GasControlData)
    );

    last_send_status_ = result;
    return result;
}

// ============================================================================
// sendGasPIDConfig — отправка PID конфига на газовый блок
// ============================================================================

esp_err_t ESPNowHandler::sendGasPIDConfig(const GasPIDConfig* config) {
    if (!initialized_ || !gas_peer_added_) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = esp_now_send(
        gas_peer_mac_,
        (const uint8_t*)config,
        sizeof(GasPIDConfig)
    );

    last_send_status_ = result;
    Serial.printf("[ESPNOW] PID config sent: Kp=%.2f Ki=%.2f Kd=%.2f SP=%.1f\n",
                  config->kp, config->ki, config->kd, config->setpoint);
    return result;
}

// ============================================================================
// getGasStatus — получить ответ от Gas Block
// ============================================================================

bool ESPNowHandler::getGasStatus(GasStatusData* out_buf) {
    if (!out_buf) return false;

    if (has_gas_data_) {
        memcpy(out_buf, &last_gas_status_, sizeof(GasStatusData));
        has_gas_data_ = false;
        return true;
    }
    return false;
}

// ============================================================================
// getLevelData — получить данные от Level модуля
// ============================================================================

bool ESPNowHandler::getLevelData(LevelModuleData* out_buf) {
    if (!out_buf) return false;

    if (has_level_data_) {
        memcpy(out_buf, &last_level_data_, sizeof(LevelModuleData));
        has_level_data_ = false;
        return true;
    }
    return false;
}

// ============================================================================
// onReceive (ISR callback!)
// ============================================================================

void ESPNowHandler::onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    ESPNowHandler& self = getInstance();

    // Определяем источник по MAC
    bool is_gas = memcmp(mac, self.gas_peer_mac_, 6) == 0;
    bool is_level = self.level_peer_added_ && memcmp(mac, self.level_peer_mac_, 6) == 0;

    if (is_gas && len == sizeof(GasStatusData)) {
        memcpy((void*)&self.last_gas_status_, data, sizeof(GasStatusData));
        self.has_gas_data_ = true;
    } else if (is_level && len == sizeof(LevelModuleData)) {
        memcpy((void*)&self.last_level_data_, data, sizeof(LevelModuleData));
        self.has_level_data_ = true;
    }
}

// ============================================================================
// onSent (callback отправки)
// ============================================================================

void ESPNowHandler::onSent(const uint8_t* mac, esp_now_send_status_t status) {
    ESPNowHandler& self = getInstance();

    if (status == ESP_NOW_SEND_SUCCESS) {
        self.last_send_status_ = ESP_OK;
    } else {
        self.last_send_status_ = ESP_FAIL;
    }
}
