/**
 * @file phase_selector.cpp
 * @brief Реализация выбора фаз по ΔT.
 *
 * Логика переходов (с учётом гистерезиса):
 *
 *   ΔT > heads_end                     → HEATING (колонна ещё не вышла на режим)
 *   body_end ≤ ΔT ≤ heads_end          → зависит от текущей фазы:
 *     - Если current_phase == HEATING  → HEADS (начало отбора голов)
 *     - Если current_phase == HEADS    → BODY (ΔT упал до зоны тела)
 *     - Иначе                          → текущая фаза сохраняется
 *   ΔT < body_end                      → TAILS (хвосты)
 *   ΔT ≈ 0 (ниже минимального порога)  → FINISH
 *
 * Гистерезис:
 *   При переходе HEADS → BODY: ΔT должен быть ≤ (heads_end - hysteresis)
 *   При переходе BODY → TAILS: ΔT должен быть ≤ (body_end - hysteresis)
 *   Это предотвращает «скачки» при ΔT ≈ порог.
 */

#include "phase_selector.h"
#include <Arduino.h>

// ============================================================================
// Singleton
// ============================================================================

PhaseSelector& PhaseSelector::getInstance() {
    static PhaseSelector instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

void PhaseSelector::begin(SettingsManager& settings) {
    settings_ = &settings;
    refreshThresholds();

    Serial.printf("[PHASE] Initialized: heads_end=%.2f, body_end=%.2f, hysteresis=%.2f\n",
                  thresholds_.heads_end, thresholds_.body_end, thresholds_.hysteresis);
}

// ============================================================================
// update
// ============================================================================

DistillPhase PhaseSelector::update(float t_select, float t_below, DistillPhase current_phase) {
    // Не обновляем если авария — фаза锁定
    if (current_phase == DistillPhase::STATE_ERROR) {
        delta_t_ = 0.0f;
        return DistillPhase::STATE_ERROR;
    }

    // Не обновляем если FINISH — процесс завершён
    if (current_phase == DistillPhase::FINISH) {
        delta_t_ = t_select - t_below;
        return DistillPhase::FINISH;
    }

    delta_t_ = t_select - t_below;

    // Проверяем, есть ли валидные данные с обоих датчиков
    if (t_select == 0.0f && t_below == 0.0f) {
        // Оба датчика не готовы — сохраняем текущую фазу
        return current_phase;
    }

    // Рассчитываем фазу с учётом гистерезиса
    DistillPhase new_phase = current_phase;
    float hys = thresholds_.hysteresis;

    if (delta_t_ > thresholds_.heads_end) {
        // ΔT слишком большой — колонна на разгоне
        new_phase = DistillPhase::HEATING;

    } else if (delta_t_ >= thresholds_.body_end && delta_t_ <= thresholds_.heads_end) {
        // ΔT в рабочей зоне — определяем по истории
        if (current_phase == DistillPhase::HEATING) {
            // Переход с HEATING: ΔT упал в зону голов → начинаем отбор
            new_phase = DistillPhase::HEADS;
        } else if (current_phase == DistillPhase::HEADS) {
            // Были в головах, ΔT в зоне тела → переходим к телу
            // С гистерезисом: только если ΔT достаточно близко к body_end
            if (delta_t_ <= thresholds_.heads_end - hys) {
                new_phase = DistillPhase::BODY;
            }
            // Иначе — остаёмся в HEADS (гистерезис)
        } else if (current_phase == DistillPhase::BODY || current_phase == DistillPhase::TAILS) {
            // Уже в теле или хвостах — сохраняем
            new_phase = current_phase;
        } else {
            // IDLE или неизвестная — переходим в HEADS
            new_phase = DistillPhase::HEADS;
        }

    } else if (delta_t_ < thresholds_.body_end) {
        // ΔT упал ниже порога тела — хвосты
        if (current_phase == DistillPhase::BODY) {
            // Переход BODY → TAILS с гистерезисом
            if (delta_t_ < thresholds_.body_end - hys) {
                new_phase = DistillPhase::TAILS;
            }
            // Иначе — гистерезис, остаёмся в BODY
        } else if (current_phase == DistillPhase::HEADS) {
            // Пропустили тело — сразу в хвосты (редкий случай)
            new_phase = DistillPhase::TAILS;
        } else {
            new_phase = DistillPhase::TAILS;
        }
    }

    // Проверка FINISH: ΔT ≈ 0 (колонна «захлебнулась» или пустой куб)
    if (delta_t_ > -0.05f && delta_t_ < 0.05f &&
        (current_phase == DistillPhase::TAILS || current_phase == DistillPhase::BODY)) {
        // ΔT стабилизировался около нуля — процесс завершён
        new_phase = DistillPhase::FINISH;
    }

    if (new_phase != current_phase) {
        Serial.printf("[PHASE] Transition: %s → %s (ΔT=%.3f)\n",
                      phaseToString(current_phase),
                      phaseToString(new_phase),
                      delta_t_);
    }

    return new_phase;
}

// ============================================================================
// calculatePhase
// ============================================================================

DistillPhase PhaseSelector::calculatePhase(float t_select, float t_below) const {
    float dt = t_select - t_below;

    if (dt > thresholds_.heads_end) {
        return DistillPhase::HEATING;
    } else if (dt >= thresholds_.body_end) {
        return DistillPhase::HEADS; // По умолчанию — головы (без истории)
    } else if (dt > 0.05f) {
        return DistillPhase::TAILS;
    } else {
        return DistillPhase::FINISH;
    }
}

// ============================================================================
// forcePhase
// ============================================================================

void PhaseSelector::forcePhase(DistillPhase phase) {
    Serial.printf("[PHASE] Force phase: %s\n", phaseToString(phase));
    // Гистерезис не сбрасываем — пороги те же
}

// ============================================================================
// refreshThresholds
// ============================================================================

void PhaseSelector::refreshThresholds() {
    if (!settings_) return;

    const SystemConfig& cfg = settings_->getConfig();
    thresholds_.heads_end = cfg.threshold_heads_end;
    thresholds_.body_end = cfg.threshold_body_end;
    thresholds_.hysteresis = cfg.delta_hysteresis;
}
