/**
 * @file state_machine.cpp
 * @brief Реализация конечного автомата: дистилляция + ректификация.
 *
 * DISTILLATION:
 *   IDLE → HEATING → FINISH
 *   - PID по температуре пара (boiler или column_top датчик)
 *   - PID по температуре конденсата (cooler датчик)
 *   - Автозавершение при падении температуры
 *
 * RECTIFICATION:
 *   IDLE → HEATING → HEADS → BODY → TAILS → FINISH
 *   - Фазы по ΔT между head_selection и body_selection
 *   - PID по температуре колонны
 *
 * PID dt измеряется через millis() — НЕ захардкожен (исправлено v1.1).
 */

#include "core/state_machine.h"
#include "core/settings.h"
#include "control/phase_selector.h"
#include <Arduino.h>

// ============================================================================
// Singleton
// ============================================================================

StateMachine& StateMachine::getInstance() {
    static StateMachine instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool StateMachine::begin(IHeater& heater, DS18B20Manager& sensors, SettingsManager& settings) {
    Serial.println("[STATE] begin()");

    heater_ = &heater;
    sensors_ = &sensors;
    settings_ = &settings;

    // Загружаем режим из конфига
    const auto& cfg = settings.getConfig();
    mode_ = cfg.mode;

    // Инициализируем PhaseSelector (только для ректификации)
    phase_selector_ = &PhaseSelector::getInstance();
    phase_selector_->begin(settings);

    // Сообщаем нагревателю о режиме
    heater_->setOperationMode(mode_);
    heater_->setCoolerType(cfg.cooler_type);

    phase_ = DistillPhase::IDLE;
    phase_start_ms_ = millis();

    pid_cube_integral_ = 0.0f;
    pid_cooler_integral_ = 0.0f;
    pid_last_update_ms_ = millis();

    Serial.printf("[STATE] Mode: %s, Phase: %s\n",
                  modeToString(mode_), phaseToString(phase_));
    return true;
}

// ============================================================================
// setMode
// ============================================================================

void StateMachine::setMode(OperationMode mode) {
    if (mode_ == mode) return;

    Serial.printf("[STATE] Mode change: %s -> %s\n",
                  modeToString(mode_), modeToString(mode));

    mode_ = mode;
    heater_->setOperationMode(mode);

    // Сброс фазы в IDLE при смене режима
    if (phase_ != DistillPhase::IDLE && phase_ != DistillPhase::STATE_ERROR) {
        exitPhase(phase_);
        phase_ = DistillPhase::IDLE;
        heater_->setPower(0);
        heater_->enableHeating(false);
    }

    // Сброс PID
    pid_cube_integral_ = 0.0f;
    pid_cooler_integral_ = 0.0f;
    pid_cube_last_error_ = 0.0f;
    pid_cooler_last_error_ = 0.0f;
    pid_last_update_ms_ = millis();
}

// ============================================================================
// update — главный метод
// ============================================================================

void StateMachine::update(const SystemSnapshot& snapshot) {
    // STATE_ERROR — блокировка
    if (phase_ == DistillPhase::STATE_ERROR) {
        return;
    }

    // Калибровка — блокировка
    if (calibrating_) {
        calibrating_ = sensors_->isCalibrating();
        return;
    }

    // Диспетчеризация по режиму
    if (mode_ == OperationMode::DISTILLATION) {
        updateDistillation(snapshot);
    } else {
        updateRectification(snapshot);
    }
}

// ============================================================================
// updateDistillation — логика простого перегона
// ============================================================================

void StateMachine::updateDistillation(const SystemSnapshot& snapshot) {
    const auto& cfg = settings_->getConfig();

    switch (phase_) {
        case DistillPhase::IDLE:
            heater_->setPower(0);
            break;

        case DistillPhase::HEATING: {
            // Ищем датчик boiler или column_top — температура пара
            const SensorData* s_boiler = sensors_->getSensorByRole(SensorRole::boiler);
            if (s_boiler == nullptr) {
                s_boiler = sensors_->getSensorByRole(SensorRole::column_top);
            }

            if (s_boiler == nullptr || !s_boiler->present || !s_boiler->active) {
                Serial.println("[STATE] DIST: No active boiler sensor, heating disabled");
                heater_->setPower(0);
                break;
            }

            float current_temp = s_boiler->temp_corrected;

            // Проверяем падение температуры (признак окончания)
            if (dist_prev_temp_ > 0.0f && current_temp < dist_prev_temp_ - 2.0f) {
                // Температура упала на 2°C — возможно, куб выкипел
                if (!dist_temp_dropping_) {
                    dist_temp_drop_start_ms_ = millis();
                    dist_temp_dropping_ = true;
                    Serial.printf("[STATE] DIST: Temp drop detected: %.1f -> %.1f\n",
                                  dist_prev_temp_, current_temp);
                } else if (millis() - dist_temp_drop_start_ms_ > 60000) {
                    // Падение длится более 1 минуты — завершаем
                    Serial.println("[STATE] DIST: Temperature drop > 1 min, switching to FINISH");
                    exitPhase(phase_);
                    phase_ = DistillPhase::FINISH;
                    enterPhase(phase_);
                    break;
                }
            } else {
                dist_temp_dropping_ = false;
            }
            dist_prev_temp_ = current_temp;

            // PID по температуре пара
            updatePIDCube(cfg.dist_target_temp, current_temp);

            // PID охладителя (конденсатор)
            const SensorData* s_cooler = sensors_->getSensorByRole(SensorRole::cooler);
            if (s_cooler != nullptr && s_cooler->present && s_cooler->active) {
                updatePIDCooler(25.0f, s_cooler->temp_corrected);  // TODO: целевая из конфига
            }
            break;
        }

        case DistillPhase::FINISH:
            heater_->setPower(0);
            break;

        default:
            break;
    }
}

// ============================================================================
// updateRectification — логика ректификационной колонны
// ============================================================================

void StateMachine::updateRectification(const SystemSnapshot& snapshot) {
    // Получаем данные датчиков
    const auto* s_head = sensors_->getSensorByRole(SensorRole::head_selection);
    const auto* s_below = sensors_->getSensorByRole(SensorRole::body_selection);

    if (s_head == nullptr || s_below == nullptr ||
        !s_head->present || !s_below->present ||
        !s_head->active || !s_below->active) {
        return;  // Нет нужных датчиков — ждём
    }

    // Обновляем фазовый селектор
    DistillPhase new_phase = phase_selector_->update(
        s_head->temp_corrected,
        s_below->temp_corrected,
        phase_
    );

    // Переход фазы
    if (new_phase != phase_) {
        exitPhase(phase_);
        phase_ = new_phase;
        enterPhase(phase_);
    }

    // Логика по фазам
    switch (phase_) {
        case DistillPhase::IDLE:
            heater_->setPower(0);
            break;

        case DistillPhase::HEATING:
            // Разгон — PID по температуре head_selection, цель 78°C
            if (s_head->temp_corrected >= 78.0f) {
                // Дошли до рабочей температуры — переход в HEADS
                phase_ = DistillPhase::HEADS;
                enterPhase(phase_);
            } else {
                updatePIDCube(78.0f, s_head->temp_corrected);
            }
            break;

        case DistillPhase::HEADS:
        case DistillPhase::BODY:
        case DistillPhase::TAILS: {
            // Поддержание температуры колонны через PID
            // Setpoint можно настраивать — пока 78°C как базовая
            updatePIDCube(78.0f, s_head->temp_corrected);

            // PID охладителя (дефлегматор)
            const SensorData* s_cooler = sensors_->getSensorByRole(SensorRole::cooler);
            if (s_cooler != nullptr && s_cooler->present && s_cooler->active) {
                updatePIDCooler(25.0f, s_cooler->temp_corrected);
            }
            break;
        }

        case DistillPhase::FINISH:
            heater_->setPower(0);
            break;

        default:
            break;
    }
}

// ============================================================================
// setPhase
// ============================================================================

void StateMachine::setPhase(DistillPhase phase) {
    if (phase_ == DistillPhase::STATE_ERROR) {
        return;
    }
    exitPhase(phase_);
    phase_ = phase;
    enterPhase(phase_);
}

// ============================================================================
// nextPhaseManual
// ============================================================================

void StateMachine::nextPhaseManual() {
    if (phase_ == DistillPhase::STATE_ERROR) {
        return;
    }
    
    DistillPhase next = DistillPhase::IDLE;
    switch (phase_) {
        case DistillPhase::IDLE:
            next = DistillPhase::HEATING;
            break;
        case DistillPhase::HEATING:
            next = DistillPhase::HEADS;
            break;
        case DistillPhase::HEADS:
            next = DistillPhase::BODY;
            break;
        case DistillPhase::BODY:
            next = DistillPhase::TAILS;
            break;
        case DistillPhase::TAILS:
            next = DistillPhase::FINISH;
            break;
        default:
            break;
    }
    
    if (next != DistillPhase::IDLE) {
        Serial.printf("[STATE] Manual next phase: %s -> %s\n",
                     phaseToString(phase_), phaseToString(next));
        setPhase(next);
    }
}

// ============================================================================
// start
// ============================================================================

void StateMachine::start() {
    if (phase_ != DistillPhase::IDLE) {
        Serial.println("[STATE] WARNING: can only start from IDLE");
        return;
    }
    phase_ = DistillPhase::HEATING;
    dist_temp_dropping_ = false;
    dist_prev_temp_ = 0.0f;
    enterPhase(phase_);
    Serial.printf("[STATE] Started: %s\n", modeToString(mode_));
}

// ============================================================================
// stop
// ============================================================================

void StateMachine::stop() {
    if (phase_ == DistillPhase::STATE_ERROR) {
        return;
    }
    exitPhase(phase_);
    phase_ = DistillPhase::IDLE;
    enterPhase(phase_);
    Serial.println("[STATE] Stopped");
}

// ============================================================================
// emergencyStop
// ============================================================================

void StateMachine::emergencyStop() {
    heater_->emergencyStop();
    phase_ = DistillPhase::STATE_ERROR;
    Serial.println("[STATE] EMERGENCY STOP");
}

// ============================================================================
// resetError
// ============================================================================

void StateMachine::resetError() {
    phase_ = DistillPhase::IDLE;
    heater_->resetError();
    heater_->setPower(0);

    pid_cube_integral_ = 0.0f;
    pid_cooler_integral_ = 0.0f;
    pid_cube_last_error_ = 0.0f;
    pid_cooler_last_error_ = 0.0f;
    pid_last_update_ms_ = millis();

    dist_temp_dropping_ = false;
    dist_prev_temp_ = 0.0f;

    Serial.println("[STATE] Error reset, phase = IDLE");
}

// ============================================================================
// startCalibration
// ============================================================================

void StateMachine::startCalibration() {
    if (!sensors_->startCalibration()) {
        Serial.println("[STATE] WARNING: calibration already in progress or failed");
        return;
    }
    calibrating_ = true;
    Serial.println("[STATE] Calibration started");
}

// ============================================================================
// setRectSubMode
// ============================================================================

void StateMachine::setRectSubMode(RectSubMode sub_mode) {
    Serial.printf("[STATE] Rect sub-mode: %s\n", subModeToString(sub_mode));
    // Подрежим влияет на логику PID охладителя
    // vapor: PID управляет дефлегматором (reflux ratio паром)
    // liquid: PID управляет жидкостным клапаном
}

// ============================================================================
// enterPhase
// ============================================================================

void StateMachine::enterPhase(DistillPhase new_phase) {
    phase_start_ms_ = millis();

    if (new_phase == DistillPhase::IDLE) {
        heater_->setPower(0);
        heater_->enableHeating(false);
    } else if (new_phase == DistillPhase::FINISH) {
        heater_->setPower(0);
        heater_->enableHeating(false);
    } else if (new_phase != DistillPhase::STATE_ERROR) {
        heater_->enableHeating(true);
    }

    Serial.printf("[STATE] Enter phase: %s\n", phaseToString(new_phase));
}

// ============================================================================
// exitPhase
// ============================================================================

void StateMachine::exitPhase(DistillPhase old_phase) {
    (void)old_phase;
    Serial.printf("[STATE] Exit phase: %s\n", phaseToString(old_phase));
}

// ============================================================================
// updatePIDCube — PID нагревателя с РЕАЛЬНЫМ dt (исправлено v1.1)
// ============================================================================

void StateMachine::updatePIDCube(float setpoint, float actual) {
    const auto& cfg = settings_->getConfig();
    float error = setpoint - actual;

    // Измеряем реальное dt (исправлено v1.1)
    uint32_t now = millis();
    float dt_sec = (now - pid_last_update_ms_) / 1000.0f;
    pid_last_update_ms_ = now;

    // Защита от слишком маленького dt
    if (dt_sec < 0.01f) {
        // Слишком часто — используем предыдущее значение
        return;
    }

    // Integral с anti-windup
    pid_cube_integral_ += error * dt_sec;
    if (pid_cube_integral_ > cfg.pid_cube.integral_max) {
        pid_cube_integral_ = cfg.pid_cube.integral_max;
    }
    if (pid_cube_integral_ < -cfg.pid_cube.integral_max) {
        pid_cube_integral_ = -cfg.pid_cube.integral_max;
    }

    // Derivative (с делением на dt — ИСПРАВЛЕНО v1.1)
    float derivative = (dt_sec > 0.001f) ? (error - pid_cube_last_error_) / dt_sec : 0.0f;
    pid_cube_last_error_ = error;

    // PID выход
    float output = cfg.pid_cube.kp * error +
                   cfg.pid_cube.ki * pid_cube_integral_ +
                   cfg.pid_cube.kd * derivative;

    // Ограничение
    if (output > cfg.pid_cube.out_max) output = cfg.pid_cube.out_max;
    if (output < cfg.pid_cube.out_min) output = cfg.pid_cube.out_min;

    pid_cube_out_ = output;
    heater_->setPower((uint8_t)output);
}

// ============================================================================
// updatePIDCooler — PID охладителя с РЕАЛЬНЫМ dt (исправлено v1.1)
// ============================================================================

void StateMachine::updatePIDCooler(float setpoint, float actual) {
    const auto& cfg = settings_->getConfig();
    float error = setpoint - actual;

    // Измеряем реальное dt (исправлено v1.1)
    uint32_t now = millis();
    float dt_sec = (now - pid_last_update_ms_) / 1000.0f;
    // pid_last_update_ms_ уже обновлён в updatePIDCube

    if (dt_sec < 0.01f) return;

    pid_cooler_integral_ += error * dt_sec;
    if (pid_cooler_integral_ > cfg.pid_cooler.integral_max) {
        pid_cooler_integral_ = cfg.pid_cooler.integral_max;
    }
    if (pid_cooler_integral_ < -cfg.pid_cooler.integral_max) {
        pid_cooler_integral_ = -cfg.pid_cooler.integral_max;
    }

    // Derivative (с делением на dt — ИСПРАВЛЕНО v1.1)
    float derivative = (dt_sec > 0.001f) ? (error - pid_cooler_last_error_) / dt_sec : 0.0f;
    pid_cooler_last_error_ = error;

    float output = cfg.pid_cooler.kp * error +
                   cfg.pid_cooler.ki * pid_cooler_integral_ +
                   cfg.pid_cooler.kd * derivative;

    if (output > cfg.pid_cooler.out_max) output = cfg.pid_cooler.out_max;
    if (output < cfg.pid_cooler.out_min) output = cfg.pid_cooler.out_min;

    pid_cooler_out_ = output;
    // Применяем к нагревателю
    heater_->setCoolerPIDParams(cfg.pid_cooler);
    heater_->updateCoolerPID(setpoint, actual);
}
