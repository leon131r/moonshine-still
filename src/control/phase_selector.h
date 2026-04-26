/**
 * @file phase_selector.h
 * @brief Автоматический выбор фаз ректификации по ΔT.
 *
 * Алгоритм:
 * - ΔT = T_head_selection - T_body_selection
 *   (разница температур между узлом отбора голов и узлом отбора тела)
 *
 * Переходы фаз:
 * - ΔT > heads_end              → PHASE_HEATING (разгон, ещё не добрали)
 * - body_end ≤ ΔT ≤ heads_end   → PHASE_HEADS или PHASE_BODY (зависит от истории)
 * - ΔT < body_end               → PHASE_TAILS
 * - ΔT ≈ 0                      → FINISH (колонна «захлебнулась»)
 *
 * Гистерезис предотвращает «дребезг» фаз при колебаниях ΔT.
 * Пороги настраиваются в settings.json и могут меняться из UI.
 *
 * Модуль НЕ управляет нагревом — только определяет текущую фазу.
 */

#ifndef PHASE_SELECTOR_H
#define PHASE_SELECTOR_H

#include "core/config.h"
#include "core/settings.h"

/**
 * @brief Класс выбора фазы по ΔT.
 *
 * Неблокирующий. Вызывать из loop с актуальными температурами.
 * Гистерезис применяется только при ПЕРЕХОДЕ между фазами,
 * не при определении текущей.
 */
class PhaseSelector {
public:
    /** @brief Получить экземпляр (singleton) */
    static PhaseSelector& getInstance();

    /**
     * @brief Инициализация.
     * @param settings Ссылка на менеджер настроек
     */
    void begin(SettingsManager& settings);

    /**
     * @brief Обновить фазу на основе текущих температур.
     * @param t_select    Температура head_selection (°C)
     * @param t_below     Температура body_selection (°C)
     * @param current_phase Текущая фаза (для гистерезиса)
     * @return Новая фаза (может совпадать с current_phase)
     *
     * Логика с гистерезисом:
     * - При ΔT в зоне неопределённости (±hysteresis) фаза НЕ меняется
     * - Это предотвращает частые переключения при колебаниях
     */
    DistillPhase update(float t_select, float t_below, DistillPhase current_phase);

    /** @brief Получить текущее значение ΔT */
    float getDeltaT() const { return delta_t_; }

    /**
     * @brief Рассчитать фазу без изменения состояния.
     * @param t_select Температура head_selection
     * @param t_below  Температура body_selection
     * @return Рассчитанная фаза
     *
     * Используется для предпросмотра в UI.
     */
    DistillPhase calculatePhase(float t_select, float t_below) const;

    /**
     * @brief Принудительно установить фазу.
     * @param phase Новая фаза
     *
     * Вызывается из UI при ручном управлении.
     */
    void forcePhase(DistillPhase phase);

    /** @brief Порог окончания голов */
    float getThresholdHeadsEnd() const { return thresholds_.heads_end; }

    /** @brief Порог окончания тела */
    float getThresholdBodyEnd() const { return thresholds_.body_end; }

    /** @brief Гистерезис */
    float getHysteresis() const { return thresholds_.hysteresis; }

    /**
     * @brief Обновить пороги из настроек.
     * Вызывается при изменении settings.json.
     */
    void refreshThresholds();

private:
    PhaseSelector() = default;
    PhaseSelector(const PhaseSelector&) = delete;
    PhaseSelector& operator=(const PhaseSelector&) = delete;

    struct Thresholds {
        float heads_end = 1.2f;
        float body_end = 0.3f;
        float hysteresis = 0.1f;
    };

    Thresholds thresholds_;
    SettingsManager* settings_ = nullptr;
    float delta_t_ = 0.0f;
};

#endif // PHASE_SELECTOR_H
