/**
 * @file electric_heater.cpp
 * @brief Реализация электрического нагревателя с двумя PID-контурами.
 *
 * PID-формула (позиционный алгоритм):
 *   error = setpoint - current
 *   integral += error * dt           (dt измеряется через millis()!)
 *   derivative = (error - prev_error) / dt
 *   output = Kp * error + Ki * integral + Kd * derivative
 *
 * Anti-windup: integral ограничена integral_max, откат при насыщении.
 * Выход ограничен out_min..out_max.
 *
 * ШИМ: ledcWrite(channel, duty), duty = power * 255 / 100.
 * Частота 1000 Гц — подходит для SSR (твердотельных реле).
 *
 * v1.1: dt измеряется через millis() — НЕ захардкожен.
 */

#include "electric_heater.h"
#include "core/settings.h"
#include <Arduino.h>

// LEDC настройки
static constexpr uint32_t PWM_FREQ = 1000;    ///< Частота ШИМ (Гц)
static constexpr uint8_t  PWM_BITS = 8;        ///< Разрешение (8 бит = 0-255)
static constexpr uint32_t PWM_MAX  = 255;      ///< Максимальный duty cycle
static constexpr uint32_t PID_MIN_DT_MS = 10;  ///< Минимальный dt для PID (защита от деления на 0)

// ============================================================================
// Singleton
// ============================================================================

ElectricHeater& ElectricHeater::getInstance() {
    static ElectricHeater instance;
    return instance;
}

// ============================================================================
// begin (без параметров — заглушка интерфейса)
// ============================================================================

bool ElectricHeater::begin() {
    Serial.println("[EHEATER] WARNING: begin() без параметров — используйте begin(pin_pwm, pin_cooler, ...)");
    return false;
}

// ============================================================================
// begin (полная инициализация)
// ============================================================================

bool ElectricHeater::begin(uint8_t pin_pwm, uint8_t pin_cooler,
                           const PIDParams& pid_cube, const PIDParams& pid_cooler,
                           const PIDParams& pid_dist_cooler) {
    Serial.printf("[EHEATER] Initializing: PWM=%d, Cooler=%d\n", pin_pwm, pin_cooler);

    // Валидация GPIO — ESP32-S3: валидны пины 0-48
    if (pin_pwm > 48) {
        Serial.printf("[EHEATER] ERROR: Invalid PWM pin: %d\n", pin_pwm);
        return false;
    }
    if (pin_cooler > 48) {
        Serial.printf("[EHEATER] ERROR: Invalid Cooler pin: %d\n", pin_cooler);
        return false;
    }

    pin_pwm_ = pin_pwm;
    pin_cooler_ = pin_cooler;
    pid_cube_params_ = pid_cube;
    pid_cooler_params_ = pid_cooler;  // rectification: дефлегматор
    // pid_dist_cooler пока не используем — будет переключён при смене режима

    // Настраиваем LEDC каналы (ESP32 Arduino legacy API)
    // Канал 0 — нагреватель куба
    if (ledcSetup(channel_cube_, PWM_FREQ, PWM_BITS) == 0) {
        Serial.printf("[EHEATER] ledcSetup failed for channel %d\n", channel_cube_);
    }
    ledcAttachPin(pin_pwm_, channel_cube_);
    ledcWrite(channel_cube_, 0);
    Serial.printf("[EHEATER] Cube PWM: ch=%d, pin=%d, freq=%dHz\n",
                  channel_cube_, pin_pwm_, PWM_FREQ);

    // Канал 1 — охладитель
    if (ledcSetup(channel_cooler_, PWM_FREQ, PWM_BITS) == 0) {
        Serial.printf("[EHEATER] ledcSetup failed for channel %d\n", channel_cooler_);
    }
    ledcAttachPin(pin_cooler_, channel_cooler_);
    ledcWrite(channel_cooler_, 0);
    Serial.printf("[EHEATER] Cooler PWM: ch=%d, pin=%d, freq=%dHz\n",
                  channel_cooler_, pin_cooler_, PWM_FREQ);

    // Начальное состояние: выключено
    heating_enabled_ = false;
    emergency_stop_ = false;
    heater_power_ = 0;

    // PID timestamp — инициализируем текущим временем
    pid_cube_last_ms_ = millis();
    pid_cooler_last_ms_ = millis();

    Serial.println("[EHEATER] Initialized successfully");
    return true;
}

// ============================================================================
// setPower
// ============================================================================

void ElectricHeater::setPower(uint8_t power) {
    if (emergency_stop_) {
        return; // Аварийная остановка — игнорируем
    }

    if (power > 100) {
        power = 100;
    }

    heater_power_ = power;

    if (heating_enabled_) {
        applyPWM(power, pin_pwm_);
    } else {
        applyPWM(0, pin_pwm_);
    }
}

// ============================================================================
// enableHeating
// ============================================================================

void ElectricHeater::enableHeating(bool enable) {
    if (emergency_stop_) {
        return; // Аварийная остановка — игнорируем
    }

    heating_enabled_ = enable;

    if (!enable) {
        heater_power_ = 0;
        applyPWM(0, pin_pwm_);
    }
}

// ============================================================================
// emergencyStop
// ============================================================================

void ElectricHeater::emergencyStop() {
    emergency_stop_ = true;
    heating_enabled_ = false;
    heater_power_ = 0;
    pid_cube_output_ = 0.0f;
    pid_cooler_output_ = 0.0f;

    // Обнуляем оба канала
    applyPWM(0, pin_pwm_);
    applyPWM(0, pin_cooler_);

    // Сбрасываем интегралы PID
    pid_cube_integral_ = 0.0f;
    pid_cooler_integral_ = 0.0f;
    pid_cube_prev_error_ = 0.0f;
    pid_cooler_prev_error_ = 0.0f;

    Serial.println("[EHEATER] EMERGENCY STOP — all outputs disabled");
}

// ============================================================================
// resetError
// ============================================================================

bool ElectricHeater::resetError() {
    if (!emergency_stop_) {
        return false; // Нет аварии для сброса
    }

    emergency_stop_ = false;
    heating_enabled_ = false; // Оставляем выключенным — пользователь должен явно включить
    heater_power_ = 0;

    Serial.println("[EHEATER] Error reset — heating remains OFF until explicitly enabled");
    return true;
}

// ============================================================================
// v1.1: Режим и конфигурация
// ============================================================================

void ElectricHeater::setOperationMode(OperationMode mode) {
    const auto& cfg = SettingsManager::getInstance().getConfig();
    op_mode_ = mode;

    // В зависимости от режима выбираем PID охладителя
    if (mode == OperationMode::DISTILLATION) {
        pid_cooler_params_ = cfg.pid_dist_cooler;  // конденсатор
        Serial.println("[EHEATER] Mode: DISTILLATION — using pid_dist_cooler");
    } else {
        pid_cooler_params_ = cfg.pid_cooler;  // дефлегматор
        Serial.println("[EHEATER] Mode: RECTIFICATION — using pid_cooler");
    }

    // Сбрасываем интегралы при смене режима
    resetPIDIntegrals();
}

void ElectricHeater::setCoolerType(CoolerType type) {
    cooler_type_ = type;
    Serial.printf("[EHEATER] Cooler type: %s\n", coolerTypeToString(type));
}

// ============================================================================
// v1.1: PID-параметры
// ============================================================================

void ElectricHeater::setHeaterPIDParams(const PIDParams& params) {
    pid_cube_params_ = params;
    // Сбрасываем интеграл при смене параметров
    pid_cube_integral_ = 0.0f;
    pid_cube_prev_error_ = 0.0f;
    pid_cube_last_ms_ = millis();
    Serial.printf("[EHEATER] Heater PID updated: Kp=%.2f, Ki=%.2f, Kd=%.2f\n",
                  params.kp, params.ki, params.kd);
}

void ElectricHeater::setCoolerPIDParams(const PIDParams& params) {
    pid_cooler_params_ = params;
    pid_cooler_integral_ = 0.0f;
    pid_cooler_prev_error_ = 0.0f;
    pid_cooler_last_ms_ = millis();
    Serial.printf("[EHEATER] Cooler PID updated: Kp=%.2f, Ki=%.2f, Kd=%.2f\n",
                  params.kp, params.ki, params.kd);
}

// ============================================================================
// v1.1: updateHeaterPID — PID нагревателя с реальным dt
// ============================================================================

float ElectricHeater::updateHeaterPID(float setpoint, float current_temp) {
    if (emergency_stop_) {
        return 0.0f;
    }

    heater_setpoint_ = setpoint;
    pid_cube_output_ = computePID(pid_cube_params_,
                                   pid_cube_integral_,
                                   pid_cube_prev_error_,
                                   pid_cube_last_ms_,
                                   setpoint, current_temp);

    // Применяем результат к ШИМ (если нагрев разрешён)
    if (heating_enabled_) {
        setPower((uint8_t)pid_cube_output_);
    }

    return pid_cube_output_;
}

// ============================================================================
// v1.1: updateCoolerPID — PID охладителя с реальным dt
// ============================================================================

float ElectricHeater::updateCoolerPID(float setpoint, float current_temp) {
    if (emergency_stop_) {
        return 0.0f;
    }

    pid_cooler_output_ = computePID(pid_cooler_params_,
                                     pid_cooler_integral_,
                                     pid_cooler_prev_error_,
                                     pid_cooler_last_ms_,
                                     setpoint, current_temp);

    // Применяем к каналу охладителя
    applyPWM((uint8_t)pid_cooler_output_, pin_cooler_);

    return pid_cooler_output_;
}

// ============================================================================
// v1.1: resetPIDIntegrals
// ============================================================================

void ElectricHeater::resetPIDIntegrals() {
    pid_cube_integral_ = 0.0f;
    pid_cooler_integral_ = 0.0f;
    pid_cube_prev_error_ = 0.0f;
    pid_cooler_prev_error_ = 0.0f;
    pid_cube_last_ms_ = millis();
    pid_cooler_last_ms_ = millis();
    Serial.println("[EHEATER] PID integrals reset");
}

// ============================================================================
// setManualPower
// ============================================================================

void ElectricHeater::setManualPower(uint8_t power) {
    if (emergency_stop_) {
        return;
    }
    setPower(power);
}

// ============================================================================
// computePID — универсальный PID с реальным dt (исправлено v1.1)
// ============================================================================

float ElectricHeater::computePID(const PIDParams& params, float& integral, float& prev_error,
                                  uint32_t& last_update_ms,
                                  float setpoint, float current) {
    uint32_t now = millis();
    uint32_t dt_ms = now - last_update_ms;
    last_update_ms = now;

    // Защита от слишком маленького dt
    if (dt_ms < PID_MIN_DT_MS) {
        // Возвращаем предыдущее значение, не вычисляем
        return integral > 0 ? pid_cube_output_ : 0.0f;
    }

    float dt = dt_ms / 1000.0f;  // Переводим в секунды
    float error = setpoint - current;

    // Пропорциональная составляющая
    float p_out = params.kp * error;

    // Интегральная составляющая (с anti-windup)
    integral += error * dt;
    if (integral > params.integral_max) {
        integral = params.integral_max;
    } else if (integral < -params.integral_max) {
        integral = -params.integral_max;
    }
    float i_out = params.ki * integral;

    // Дифференциальная составляющая (dt в знаменателе — ИСПРАВЛЕНО v1.1)
    float derivative = (dt > 0.001f) ? (error - prev_error) / dt : 0.0f;
    float d_out = params.kd * derivative;

    prev_error = error;

    // Суммарный выход
    float output = p_out + i_out + d_out;

    // Ограничение выхода + anti-windup при насыщении
    if (output < params.out_min) {
        output = params.out_min;
        integral -= error * dt; // Откат интеграла
    }
    if (output > params.out_max) {
        output = params.out_max;
        integral -= error * dt;
    }

    return output;
}

// ============================================================================
// applyPWM
// ============================================================================

void ElectricHeater::applyPWM(uint8_t power, uint8_t pin) {
    // Преобразуем 0-100% → 0-255
    uint32_t duty = ((uint32_t)power * PWM_MAX) / 100;
    ledcWrite(pin, duty);  // ledcWrite принимает пин (не канал!)
}
