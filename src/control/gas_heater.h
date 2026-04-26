/**
 * @file gas_heater.h
 * @brief Газовый нагреватель — управление через ESP-NOW.
 *
 * Три режима работы:
 * 1. AUTO — контроллер шлёт старт/стоп + мощность, блок сам управляет
 * 2. REMOTE_PID — контроллер шлёт PID-коэффициенты (kp, ki, kd, setpoint),
 *    блок вычисляет PID самостоятельно
 * 3. MANUAL_POWER — контроллер шлёт фиксированную мощность (ручной режим)
 *
 * Протокол:
 * 1. Контроллер заполняет GasControlData {power, enable, crc}
 *    или GasPIDConfig {kp, ki, kd, setpoint, mode, crc}
 * 2. Отправляет ESP-NOW на MAC газового блока
 * 3. Ждёт GasStatusData {ack, status, gas_temp, crc} — таймаут 500мс
 * 4. При отсутствии ack — повтор (до 3 попыток)
 * 5. При всех неудачах — ошибка в лог, но heating не блокируется
 *    (газовый блок может работать автономно)
 *
 * CRC32 обеспечивает целостность данных.
 *
 * v1.1: добавлена поддержка GasPIDConfig и GasControlMode
 */

#ifndef GAS_HEATER_H
#define GAS_HEATER_H

#include "control/heater_interface.h"
#include "core/config.h"

/**
 * @brief Газовый нагреватель через ESP-NOW.
 *
 * Не инициализирует ESP-NOW самостоятельно —
 * использует внешний ESPNowHandler (инициализация в main.cpp).
 */
class GasHeater : public IHeater {
public:
    /** @brief Получить экземпляр (singleton) */
    static GasHeater& getInstance();

    /**
     * @brief Инициализация.
     * @param mac_gas MAC-адрес газового блока (6 байт)
     * @return true при успехе
     */
    bool begin(const uint8_t mac_gas[6]);

    // --- IHeater интерфейс ---
    bool begin() override; // Перегрузка без параметров (MAC берётся из конфига)
    void setPower(uint8_t power) override;
    uint8_t getPower() const override { return heater_power_; }
    void enableHeating(bool enable) override;
    bool isHeatingEnabled() const override { return heating_enabled_; }
    void emergencyStop() override;
    bool resetError() override;
    HeaterType getType() const override { return HeaterType::GAS; }

    // --- v1.1: Режим и конфигурация ---
    void setOperationMode(OperationMode mode) override;
    OperationMode getOperationMode() const override { return op_mode_; }
    void setCoolerType(CoolerType type) override;
    CoolerType getCoolerType() const override { return CoolerType::FAN; } // Газовый блок не управляет охладителем напрямую

    // --- v1.1: PID-параметры ---
    void setHeaterPIDParams(const PIDParams& params) override;
    void setCoolerPIDParams(const PIDParams& params) override;
    float updateHeaterPID(float setpoint, float current_temp) override;
    float updateCoolerPID(float setpoint, float current_temp) override;
    float getHeaterPIDOutput() const override { return pid_heater_output_; }
    float getCoolerPIDOutput() const override { return 0.0f; } // Газовый блок не управляет охладителем
    void resetPIDIntegrals() override;

    /**
     * @brief Отправить команду мощности на газовый блок.
     * @param power  Мощность 0-100%
     * @param enable Разрешение нагрева
     * @return true при получении ack
     *
     * Выполняет до ESPNOW_MAX_RETRIES попыток.
     * При успехе обновляет last_ack_status_.
     */
    bool sendCommand(uint8_t power, bool enable);

    /**
     * @brief Отправить PID-конфиг на газовый блок (remote PID режим).
     * @param config PID-параметры + setpoint + mode
     * @return true при получении ack
     */
    bool sendPIDConfig(const GasPIDConfig& config);

    /** @brief Статус последнего ack (0=OK, >0=ошибка блока) */
    uint8_t getLastAckStatus() const { return last_ack_status_; }

    /** @brief Температура газового блока (если доступна) */
    float getGasTemp() const { return last_gas_temp_; }

    /** @brief Количество неудачных отправок подряд */
    uint8_t getConsecutiveFails() const { return consecutive_fails_; }

    /** @brief Текущий режим управления газовым блоком */
    GasControlMode getControlMode() const { return control_mode_; }

    /** @brief Установить режим управления */
    void setControlMode(GasControlMode mode);

private:
    GasHeater() = default;
    GasHeater(const GasHeater&) = delete;
    GasHeater& operator=(const GasHeater&) = delete;

    /** @brief Вычислить CRC32 для GasControlData */
    static uint32_t computeCRC(const GasControlData& data);

    /** @brief Вычислить CRC32 для GasPIDConfig */
    static uint32_t computeCRC(const GasPIDConfig& data);

    /** @brief Проверить CRC32 для GasStatusData */
    static bool verifyCRC(const GasStatusData& data);

    // Состояние
    uint8_t  mac_gas_[6] = {0};       ///< MAC газового блока
    uint8_t  heater_power_ = 0;       ///< Текущая мощность (0-100%)
    bool     heating_enabled_ = false;///< Разрешение нагрева
    bool     emergency_stop_ = false; ///< Флаг аварийной остановки
    GasControlMode control_mode_ = GasControlMode::AUTO; ///< Режим управления
    OperationMode op_mode_ = OperationMode::RECTIFICATION;

    uint8_t  last_ack_status_ = 0;    ///< Статус последнего ack
    float    last_gas_temp_ = 0.0f;   ///< Температура газового блока
    uint8_t  consecutive_fails_ = 0;  ///< Неудачные попытки подряд

    // PID нагревателя (для remote PID mode)
    PIDParams pid_heater_params_;
    float     pid_heater_output_ = 0.0f;
    float     pid_heater_integral_ = 0.0f;
    float     pid_heater_prev_error_ = 0.0f;
    uint32_t  pid_heater_last_ms_ = 0;

    // PID охладителя (не используется для газового блока, но нужен для интерфейса)
    PIDParams pid_cooler_params_;
    float     pid_cooler_output_ = 0.0f;
    float     pid_cooler_integral_ = 0.0f;
    float     pid_cooler_prev_error_ = 0.0f;
    uint32_t  pid_cooler_last_ms_ = 0;
};

#endif // GAS_HEATER_H
