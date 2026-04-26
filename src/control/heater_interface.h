/**
 * @file heater_interface.h
 * @brief Абстрактный интерфейс нагревателя.
 *
 * Определяет контракт для всех типов нагревателей:
 * - ElectricHeater — прямой ШИМ/реле на GPIO
 * - GasHeater     — управление через ESP-NOW
 *
 * Общий интерфейс позволяет коду state machine и PID-регулятора
 * работать с любым типом нагревателя без условной компиляции.
 *
 * Критически важно: emergencyStop() — мгновенное отключение.
 *
 * v1.1: добавлены методы setOperationMode(), setCoolerType(),
 *       setHeaterPIDParams(), setCoolerPIDParams().
 */

#ifndef HEATER_INTERFACE_H
#define HEATER_INTERFACE_H

#include "core/config.h"

/**
 * @brief Абстрактный интерфейс нагревателя.
 *
 * Все реализации должны обеспечить:
 * - Установку мощности (0-100%)
 * - Разрешение/запрет нагрева
 * - Мгновенную аварийную остановку
 * - Настройку режима работы (distillation/rectification)
 * - Настройку типа охладителя (fan/servo)
 * - Настройку PID-параметров
 */
class IHeater {
public:
    virtual ~IHeater() = default;

    /**
     * @brief Инициализация нагревателя.
     * @return true при успехе
     */
    virtual bool begin() = 0;

    /**
     * @brief Установить мощность нагрева.
     * @param power Мощность 0-100%
     *
     * ElectricHeater: преобразует в ШИМ (ledcWrite).
     * GasHeater: отправляет GasControlData по ESP-NOW.
     */
    virtual void setPower(uint8_t power) = 0;

    /**
     * @brief Получить текущую уставку мощности.
     * @return Мощность 0-100%
     */
    virtual uint8_t getPower() const = 0;

    /**
     * @brief Разрешить или запретить нагрев.
     * @param enable true = разрешить, false = запретить
     *
     * При enable=false мощность обнуляется автоматически.
     */
    virtual void enableHeating(bool enable) = 0;

    /** @brief true, если нагрев разрешён */
    virtual bool isHeatingEnabled() const = 0;

    /**
     * @brief Аварийная остановка.
     *
     * Мгновенно обнуляет мощность и запрещает нагрев.
     * НЕ может быть отменена автоматически — только через resetError().
     */
    virtual void emergencyStop() = 0;

    /**
     * @brief Сброс после аварийной остановки.
     * @return true при успехе (если система готова к работе)
     */
    virtual bool resetError() = 0;

    /** @brief Тип нагревателя */
    virtual HeaterType getType() const = 0;

    // ========================================================================
    // v1.1: Режим и конфигурация
    // ========================================================================

    /**
     * @brief Установить режим работы.
     * @param mode DISTILLATION или RECTIFICATION
     *
     * DISTILLATION: простой перегон, один PID по температуре
     * RECTIFICATION: колонна, фазы по ΔT
     */
    virtual void setOperationMode(OperationMode mode) = 0;

    /** @brief Получить текущий режим */
    virtual OperationMode getOperationMode() const = 0;

    /**
     * @brief Установить тип охладителя.
     * @param type FAN (вентилятор) или SERVO (сервопривод)
     *
     * Влияет на PWM-выход охладителя.
     */
    virtual void setCoolerType(CoolerType type) = 0;

    /** @brief Получить тип охладителя */
    virtual CoolerType getCoolerType() const = 0;

    // ========================================================================
    // v1.1: PID-параметры
    // ========================================================================

    /**
     * @brief Установить параметры PID нагревателя (куба).
     * @param params Параметры PID
     */
    virtual void setHeaterPIDParams(const PIDParams& params) = 0;

    /**
     * @brief Установить параметры PID охладителя.
     * @param params Параметры PID
     */
    virtual void setCoolerPIDParams(const PIDParams& params) = 0;

    /**
     * @brief Обновить PID нагревателя (вызывать периодически).
     * @param setpoint    Целевая температура
     * @param current_temp Текущая температура
     * @return Выход PID (0-100%), автоматически применяется к нагревателю
     */
    virtual float updateHeaterPID(float setpoint, float current_temp) = 0;

    /**
     * @brief Обновить PID охладителя (вызывать периодически).
     * @param setpoint     Целевая температура
     * @param current_temp Текущая температура
     * @return Выход PID (0-100%), автоматически применяется к охладителю
     */
    virtual float updateCoolerPID(float setpoint, float current_temp) = 0;

    /** @brief Получить текущий выход PID нагревателя (%) */
    virtual float getHeaterPIDOutput() const = 0;

    /** @brief Получить текущий выход PID охладителя (%) */
    virtual float getCoolerPIDOutput() const = 0;

    /** @brief Сбросить интегралы PID */
    virtual void resetPIDIntegrals() = 0;
};

#endif // HEATER_INTERFACE_H
