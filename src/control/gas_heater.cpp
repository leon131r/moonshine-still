/**
 * @file gas_heater.cpp
 * @brief Реализация газового нагревателя через ESP-NOW.
 *
 * Три режима работы:
 * 1. AUTO — шлём power + enable, блок сам управляет
 * 2. REMOTE_PID — шлём PID-коэффициенты на блок, блок сам считает PID
 * 3. MANUAL_POWER — шлём фиксированную мощность
 *
 * CRC32 для целостности данных (оба направления).
 * До 3 повторных попыток при отсутствии ack.
 * Таймаут 500 мс на ожидание ответа.
 * При всех неудачах — логирование, но не блокировка.
 *
 * v1.1: добавлена поддержка GasPIDConfig, remote PID mode
 */

#include "gas_heater.h"
#include "comm/espnow_handler.h"
#include "core/settings.h"
#include <Arduino.h>
#include <esp_now.h>

// ============================================================================
// Singleton
// ============================================================================

GasHeater& GasHeater::getInstance() {
    static GasHeater instance;
    return instance;
}

// ============================================================================
// begin (с MAC)
// ============================================================================

bool GasHeater::begin(const uint8_t mac_gas[6]) {
    Serial.printf("[GAS] Initializing: MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac_gas[0], mac_gas[1], mac_gas[2],
                  mac_gas[3], mac_gas[4], mac_gas[5]);

    memcpy(mac_gas_, mac_gas, 6);

    bool all_zero = true;
    for (int i = 0; i < 6; i++) {
        if (mac_gas_[i] != 0) { all_zero = false; break; }
    }

    if (all_zero) {
        Serial.println("[GAS] WARNING: MAC address is all zeros");
    }

    heating_enabled_ = false;
    emergency_stop_ = false;
    consecutive_fails_ = 0;
    control_mode_ = GasControlMode::AUTO;

    // PID timestamp
    pid_heater_last_ms_ = millis();
    pid_cooler_last_ms_ = millis();

    Serial.println("[GAS] Initialized");
    return true;
}

// ============================================================================
// begin (без параметров)
// ============================================================================

bool GasHeater::begin() {
    uint8_t mac[6] = {0};
    return begin(mac);
}

// ============================================================================
// setPower
// ============================================================================

void GasHeater::setPower(uint8_t power) {
    if (emergency_stop_) return;
    if (power > 100) power = 100;

    heater_power_ = power;

    // В режиме AUTO или MANUAL_POWER отправляем команду
    if (control_mode_ == GasControlMode::AUTO ||
        control_mode_ == GasControlMode::MANUAL_POWER) {
        if (heating_enabled_) {
            sendCommand(power, true);
        }
    }
    // В режиме REMOTE_PID блок сам решает мощность по PID
}

// ============================================================================
// enableHeating
// ============================================================================

void GasHeater::enableHeating(bool enable) {
    if (emergency_stop_) return;

    heating_enabled_ = enable;

    if (!enable) {
        heater_power_ = 0;
        sendCommand(0, false);
    } else {
        if (control_mode_ == GasControlMode::AUTO ||
            control_mode_ == GasControlMode::MANUAL_POWER) {
            sendCommand(heater_power_, true);
        }
    }
}

// ============================================================================
// emergencyStop
// ============================================================================

void GasHeater::emergencyStop() {
    emergency_stop_ = true;
    heating_enabled_ = false;
    heater_power_ = 0;
    sendCommand(0, false);
    Serial.println("[GAS] EMERGENCY STOP");
}

// ============================================================================
// resetError
// ============================================================================

bool GasHeater::resetError() {
    if (!emergency_stop_) return false;
    emergency_stop_ = false;
    heating_enabled_ = false;
    heater_power_ = 0;
    consecutive_fails_ = 0;
    resetPIDIntegrals();
    Serial.println("[GAS] Error reset");
    return true;
}

// ============================================================================
// v1.1: Режим и конфигурация
// ============================================================================

void GasHeater::setOperationMode(OperationMode mode) {
    op_mode_ = mode;
    Serial.printf("[GAS] Mode: %s\n", modeToString(mode));
    resetPIDIntegrals();
}

void GasHeater::setCoolerType(CoolerType type) {
    // Газовый блок не управляет охладителем напрямую
    (void)type;
    Serial.println("[GAS] WARNING: setCoolerType — not applicable for gas heater");
}

// ============================================================================
// v1.1: PID-параметры
// ============================================================================

void GasHeater::setHeaterPIDParams(const PIDParams& params) {
    pid_heater_params_ = params;
    pid_heater_integral_ = 0.0f;
    pid_heater_prev_error_ = 0.0f;
    pid_heater_last_ms_ = millis();
    Serial.printf("[GAS] Heater PID updated: Kp=%.2f, Ki=%.2f, Kd=%.2f\n",
                  params.kp, params.ki, params.kd);

    // В режиме REMOTE_PID отправляем PID на блок
    if (control_mode_ == GasControlMode::REMOTE_PID) {
        GasPIDConfig cfg;
        cfg.kp = params.kp;
        cfg.ki = params.ki;
        cfg.kd = params.kd;
        cfg.setpoint = 78.0f;  // Default setpoint
        cfg.mode = static_cast<uint8_t>(GasControlMode::REMOTE_PID);
        cfg.crc = computeCRC(cfg);
        sendPIDConfig(cfg);
    }
}

void GasHeater::setCoolerPIDParams(const PIDParams& params) {
    pid_cooler_params_ = params;
    pid_cooler_integral_ = 0.0f;
    pid_cooler_prev_error_ = 0.0f;
    pid_cooler_last_ms_ = millis();
    Serial.println("[GAS] Cooler PID set (not used for gas heater)");
}

// ============================================================================
// v1.1: updateHeaterPID — вычисляет PID для remote PID mode
// ============================================================================

float GasHeater::updateHeaterPID(float setpoint, float current_temp) {
    if (emergency_stop_) {
        return 0.0f;
    }

    // В режиме REMOTE_PID — PID вычисляется на блоке, мы только шлём конфиг
    // Но локально тоже считаем для телеметрии
    uint32_t now = millis();
    uint32_t dt_ms = now - pid_heater_last_ms_;
    pid_heater_last_ms_ = now;

    if (dt_ms < 10) return pid_heater_output_;

    float dt = dt_ms / 1000.0f;
    float error = setpoint - current_temp;

    pid_heater_integral_ += error * dt;
    if (pid_heater_integral_ > pid_heater_params_.integral_max)
        pid_heater_integral_ = pid_heater_params_.integral_max;
    if (pid_heater_integral_ < -pid_heater_params_.integral_max)
        pid_heater_integral_ = -pid_heater_params_.integral_max;

    float derivative = (dt > 0.001f) ? (error - pid_heater_prev_error_) / dt : 0.0f;
    pid_heater_prev_error_ = error;

    pid_heater_output_ = pid_heater_params_.kp * error +
                         pid_heater_params_.ki * pid_heater_integral_ +
                         pid_heater_params_.kd * derivative;

    if (pid_heater_output_ > pid_heater_params_.out_max)
        pid_heater_output_ = pid_heater_params_.out_max;
    if (pid_heater_output_ < pid_heater_params_.out_min)
        pid_heater_output_ = pid_heater_params_.out_min;

    // В режиме REMOTE_PID — обновляем блок новым setpoint
    if (control_mode_ == GasControlMode::REMOTE_PID) {
        GasPIDConfig cfg;
        cfg.kp = pid_heater_params_.kp;
        cfg.ki = pid_heater_params_.ki;
        cfg.kd = pid_heater_params_.kd;
        cfg.setpoint = setpoint;
        cfg.mode = static_cast<uint8_t>(GasControlMode::REMOTE_PID);
        cfg.crc = computeCRC(cfg);
        sendPIDConfig(cfg);
    }

    return pid_heater_output_;
}

// ============================================================================
// v1.1: updateCoolerPID — не используется для газового блока
// ============================================================================

float GasHeater::updateCoolerPID(float setpoint, float current_temp) {
    (void)setpoint;
    (void)current_temp;
    return 0.0f; // Газовый блок не управляет охладителем
}

// ============================================================================
// v1.1: resetPIDIntegrals
// ============================================================================

void GasHeater::resetPIDIntegrals() {
    pid_heater_integral_ = 0.0f;
    pid_cooler_integral_ = 0.0f;
    pid_heater_prev_error_ = 0.0f;
    pid_cooler_prev_error_ = 0.0f;
    pid_heater_last_ms_ = millis();
    pid_cooler_last_ms_ = millis();
    Serial.println("[GAS] PID integrals reset");
}

// ============================================================================
// setControlMode
// ============================================================================

void GasHeater::setControlMode(GasControlMode mode) {
    Serial.printf("[GAS] Control mode: %s -> %s\n",
                  gasControlModeToString(control_mode_),
                  gasControlModeToString(mode));

    control_mode_ = mode;
    resetPIDIntegrals();
}

// ============================================================================
// sendCommand
// ============================================================================

bool GasHeater::sendCommand(uint8_t power, bool enable) {
    GasControlData cmd;
    cmd.power = power;
    cmd.enable = enable;
    cmd.crc = computeCRC(cmd);

    bool ack_received = false;

    for (uint8_t retry = 0; retry < ESPNOW_MAX_RETRIES; retry++) {
        esp_err_t result = ESPNowHandler::getInstance().sendGasControl(&cmd);

        if (result != ESP_OK) {
            Serial.printf("[GAS] ESP-NOW send failed (attempt %d/%d): %s\n",
                          retry + 1, ESPNOW_MAX_RETRIES, esp_err_to_name(result));
            delay(50);
            continue;
        }

        uint32_t start = millis();
        while (millis() - start < ESPNOW_ACK_TIMEOUT) {
            GasStatusData status;
            if (ESPNowHandler::getInstance().getGasStatus(&status)) {
                if (verifyCRC(status)) {
                    if (status.ack) {
                        last_ack_status_ = status.status;
                        last_gas_temp_ = status.gas_temp;
                        consecutive_fails_ = 0;
                        ack_received = true;
                    }
                    break;
                }
            }
            delay(10);
        }

        if (ack_received) break;
        Serial.printf("[GAS] ACK timeout (attempt %d/%d)\n", retry + 1, ESPNOW_MAX_RETRIES);
        delay(50);
    }

    if (!ack_received) {
        consecutive_fails_++;
        Serial.printf("[GAS] All attempts failed. Fails: %d\n", consecutive_fails_);
    }

    return ack_received;
}

// ============================================================================
// sendPIDConfig — отправка PID-конфига на газовый блок
// ============================================================================

bool GasHeater::sendPIDConfig(const GasPIDConfig& config) {
    // Отправляем PID конфиг через ESPNowHandler
    esp_err_t res = ESPNowHandler::getInstance().sendGasPIDConfig(&config);
    return res == ESP_OK;
}

// ============================================================================
// computeCRC для GasControlData
// ============================================================================

uint32_t GasHeater::computeCRC(const GasControlData& data) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&data);
    // Считаем CRC для всех байт кроме самого crc (sizeof - 4)
    for (size_t i = 0; i < sizeof(GasControlData) - 4; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// computeCRC для GasPIDConfig
// ============================================================================

uint32_t GasHeater::computeCRC(const GasPIDConfig& data) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&data);
    // Считаем CRC для всех байт кроме самого crc (sizeof - 4)
    for (size_t i = 0; i < sizeof(GasPIDConfig) - 4; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// verifyCRC для GasStatusData
// ============================================================================

bool GasHeater::verifyCRC(const GasStatusData& data) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&data);
    // Считаем CRC для всех байт кроме самого crc (sizeof - 4)
    for (size_t i = 0; i < sizeof(GasStatusData) - 4; i++) {
        crc ^= bytes[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    return (crc ^ 0xFFFFFFFF) == data.crc;
}
