/**
 * @file state_machine.h
 * @brief Конечный автомат процесса: дистилляция И ректификация.
 *
 * Два режима работы:
 *
 * DISTILLATION (простой перегон):
 *   IDLE → HEATING → FINISH
 *   - Нагрев до целевой температуры (dist_target_temp)
 *   - Поддержание температуры через PID
 *   - Завершение при падении температуры или по команде
 *
 * RECTIFICATION (ректификация):
 *   IDLE → HEATING → HEADS → BODY → TAILS → FINISH
 *   - Разгон колонны
 *   - Автоматический переход фаз по ΔT
 *   - Подрежимы: vapor (отбор по пару), liquid (отбор жидкости)
 *
 * Любой режим: .any → STATE_ERROR → (Manual reset only)
 */

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "core/config.h"
#include "control/heater_interface.h"
#include "control/phase_selector.h"
#include "sensors/ds18b20_manager.h"
#include "core/settings.h"

/**
 * @brief Класс конечного автомата.
 *
 * Singleton. Координирует процесс дистилляции/ректификации.
 * Управляет нагревателем через IHeater интерфейс.
 * PID dt измеряется через millis(), НЕ захардкожен (исправлено v1.1).
 */
class StateMachine {
public:
    /**
     * @brief Получить экземпляр (singleton).
     */
    static StateMachine& getInstance();

    /**
     * @brief Инициализация.
     * @param heater   Ссылка на нагреватель
     * @param sensors  Ссылка на менеджер датчиков
     * @param settings Ссылка на настройки
     * @return true при успехе
     */
    bool begin(IHeater& heater, DS18B20Manager& sensors, SettingsManager& settings);

    /**
     * @brief Обновление состояния (вызывать из loop).
     * @param snapshot Текущий снапшот системы
     *
     * Основной метод — управляет переходами фаз в зависимости от режима.
     * В режиме DISTILLATION: PID по температуре пара.
     * В режиме RECTIFICATION: фазы по ΔT, PID по температуре колонны.
     */
    void update(const SystemSnapshot& snapshot);

    /**
     * @brief Установить режим работы.
     * @param mode DISTILLATION или RECTIFICATION
     */
    void setMode(OperationMode mode);

    /** @brief Получить текущий режим */
    OperationMode getMode() const { return mode_; }

    /**
     * @brief Получить текущую фазу.
     *
     * В режиме DISTILLATION фазы ограничены: IDLE, HEATING, FINISH, STATE_ERROR.
     */
    DistillPhase getPhase() const { return phase_; }

    /**
     * @brief Установить фазу вручную.
     */
    void setPhase(DistillPhase phase);
    
    /**
     * @brief Ручной переход к следующей фазе.
     */
    void nextPhaseManual();
    
    /**
     * @brief Запустить процесс (из IDLE).
     */
    void start();

    /**
     * @brief Остановить процесс (перейти в IDLE).
     */
    void stop();

    /**
     * @brief Аварийная остановка.
     *
     * Переход в STATE_ERROR, обнуление нагрева.
     * НЕ автоматический сброс — только вручную.
     */
    void emergencyStop();

    /**
     * @brief Сброс после аварии.
     */
    void resetError();

    /**
     * @brief Запустить калибровку.
     */
    void startCalibration();

    /** @brief true если идёт калибровка */
    bool isCalibrating() const { return calibrating_; }

    /** @brief Получить выход PID нагревателя */
    float getPIDCubeOut() const { return pid_cube_out_; }

    /** @brief Получить выход PID охладителя */
    float getPIDCoolerOut() const { return pid_cooler_out_; }

    /** @brief Установить подрежим ректификации */
    void setRectSubMode(RectSubMode sub_mode);

private:
    StateMachine() = default;
    StateMachine(const StateMachine&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;

    IHeater*           heater_ = nullptr;
    DS18B20Manager*    sensors_ = nullptr;
    SettingsManager*   settings_ = nullptr;
    PhaseSelector*     phase_selector_ = nullptr;

    OperationMode      mode_ = OperationMode::RECTIFICATION;
    DistillPhase       phase_ = DistillPhase::IDLE;

    bool calibrating_ = false;

    // PID state (с измерением dt через millis() — исправлено v1.1)
    float pid_cube_out_ = 0.0f;
    float pid_cooler_out_ = 0.0f;
    float pid_cube_integral_ = 0.0f;
    float pid_cooler_integral_ = 0.0f;
    float pid_cube_last_error_ = 0.0f;
    float pid_cooler_last_error_ = 0.0f;
    uint32_t pid_last_update_ms_ = 0;

    uint32_t phase_start_ms_ = 0;

    // --- Логика дистилляции ---
    float dist_prev_temp_ = 0.0f;
    uint32_t dist_temp_drop_start_ms_ = 0;
    bool dist_temp_dropping_ = false;

    void updatePIDCube(float setpoint, float actual);
    void updatePIDCooler(float setpoint, float actual);
    void enterPhase(DistillPhase new_phase);
    void exitPhase(DistillPhase old_phase);

    // --- DISTILLATION ---
    void updateDistillation(const SystemSnapshot& snapshot);

    // --- RECTIFICATION ---
    void updateRectification(const SystemSnapshot& snapshot);
};

#endif // STATE_MACHINE_H
