/**
 * @file electric_heater.h
 * @brief Электрический нагреватель — два PID-контура + ШИМ.
 *
 * Два независимых канала:
 * 1. PID нагревателя (куба) — управление мощностью нагрева (pin_pwm)
 *    Цель: разгон колонны и поддержание температуры кипения.
 *
 * 2. PID охладителя — поддержание температуры (pin_cooler)
 *    - Режим RECTIFICATION: дефлегматор колонны
 *    - Режим DISTILLATION: конденсатор (вентилятор на радиаторе ИЛИ сервопривод)
 *
 * ШИМ через ledc (ESP32 LED Control):
 * - Частота: 1000 Гц (для реле/SSR оптимально)
 * - Разрешение: 8 бит (0-255 → 0-100%)
 * - Два независимых канала
 *
 * PID: dt измеряется через millis(), НЕ захардкожен (исправлено v1.1).
 */

#ifndef ELECTRIC_HEATER_H
#define ELECTRIC_HEATER_H

#include "control/heater_interface.h"
#include "core/config.h"
#include <Arduino.h>

/**
 * @brief Электрический нагреватель с двумя PID-контурами.
 */
class ElectricHeater : public IHeater {
public:
    /** @brief Получить экземпляр (singleton) */
    static ElectricHeater& getInstance();

    /** @brief Инициализация (заглушка интерфейса IHeater). Используй begin(pin_pwm, pin_cooler, ...) */
    bool begin() override;

    /**
     * @brief Полная инициализация.
     * @param pin_pwm      Пин ШИМ для нагревателя куба
     * @param pin_cooler   Пин ШИМ для охладителя
     * @param pid_cube     Параметры PID нагревателя
     * @param pid_cooler   Параметры PID охладителя
     * @param pid_dist_cooler Параметры PID охладителя дистилляции
     * @return true при успехе
     *
     * Настраивает ledc на обоих пинах:
     * - Частота 1000 Гц, разрешение 8 бит
     * - Начальная мощность = 0
     */
    bool begin(uint8_t pin_pwm, uint8_t pin_cooler,
               const PIDParams& pid_cube, const PIDParams& pid_cooler,
               const PIDParams& pid_dist_cooler);

    // --- IHeater интерфейс ---
    void setPower(uint8_t power) override;
    uint8_t getPower() const override { return heater_power_; }
    void enableHeating(bool enable) override;
    bool isHeatingEnabled() const override { return heating_enabled_; }
    void emergencyStop() override;
    bool resetError() override;
    HeaterType getType() const override { return HeaterType::ELECTRIC; }

    // --- v1.1: Режим и конфигурация ---
    void setOperationMode(OperationMode mode) override;
    OperationMode getOperationMode() const override { return op_mode_; }
    void setCoolerType(CoolerType type) override;
    CoolerType getCoolerType() const override { return cooler_type_; }

    // --- v1.1: PID-параметры ---
    void setHeaterPIDParams(const PIDParams& params) override;
    void setCoolerPIDParams(const PIDParams& params) override;
    float updateHeaterPID(float setpoint, float current_temp) override;
    float updateCoolerPID(float setpoint, float current_temp) override;
    float getHeaterPIDOutput() const override { return pid_cube_output_; }
    float getCoolerPIDOutput() const override { return pid_cooler_output_; }
    void resetPIDIntegrals() override;

    /**
     * @brief Прямая установка мощности (минуя PID).
     * @param power 0-100%
     *
     * Используется в ручном режиме.
     * При вызове updateHeaterPID() значение переопределяется.
     */
    void setManualPower(uint8_t power);

    /** @brief Температура, к которой стремимся (для логов) */
    float getHeaterSetpoint() const { return heater_setpoint_; }

private:
    ElectricHeater() = default;
    ElectricHeater(const ElectricHeater&) = delete;
    ElectricHeater& operator=(const ElectricHeater&) = delete;

    /**
     * @brief Вычислить PID.
     * @param params      Параметры PID
     * @param integral    Текущее значение интеграла (ссылка, изменяется)
     * @param prev_error  Предыдущая ошибка (ссылка, изменяется)
     * @param last_update_ms Время последнего вызова (ссылка, изменяется)
     * @param setpoint    Целевое значение
     * @param current     Текущее значение
     * @return Выход PID (ограниченный out_min..out_max)
     *
     * dt измеряется через millis() — НЕ захардкожен.
     */
    float computePID(const PIDParams& params, float& integral, float& prev_error,
                     uint32_t& last_update_ms,
                     float setpoint, float current);

    /**
     * @brief Применить мощность к ШИМ.
     * @param power 0-100%
     * @param pin   Номер GPIO пина
     */
    void applyPWM(uint8_t power, uint8_t pin);

    // Пины
    uint8_t  pin_pwm_ = 0;
    uint8_t  pin_cooler_ = 0;

    // Состояние
    uint8_t  heater_power_ = 0;       ///< Текущая мощность нагрева (0-100%)
    bool     heating_enabled_ = false;///< Разрешение нагрева
    bool     emergency_stop_ = false; ///< Флаг аварийной остановки

    // Режимы
    OperationMode op_mode_ = OperationMode::RECTIFICATION;
    CoolerType    cooler_type_ = CoolerType::FAN;

    // PID нагревателя (куба)
    PIDParams pid_cube_params_;
    float     pid_cube_output_ = 0.0f;
    float     pid_cube_integral_ = 0.0f;
    float     pid_cube_prev_error_ = 0.0f;
    uint32_t  pid_cube_last_ms_ = 0;
    float     heater_setpoint_ = 0.0f;

    // PID охладителя
    PIDParams pid_cooler_params_;
    float     pid_cooler_output_ = 0.0f;
    float     pid_cooler_integral_ = 0.0f;
    float     pid_cooler_prev_error_ = 0.0f;
    uint32_t  pid_cooler_last_ms_ = 0;

    // LEDC каналы
    uint8_t  channel_cube_ = 2;
    uint8_t  channel_cooler_ = 3;
};

#endif // ELECTRIC_HEATER_H
