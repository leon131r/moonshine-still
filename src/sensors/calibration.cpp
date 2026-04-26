/**
 * @file calibration.cpp
 * @brief Реализация менеджера калибровки.
 *
 * Алгоритм детально:
 *
 * Входные данные:
 * - sensors[].calibrate — флаг автоматической калибровки
 * - averages[i] — средняя температура i-го датчика за время калибровки
 *
 * Шаг 1: Суммируем averages только для датчиков с calibrate=true
 * Шаг 2: global_mean = sum / count_calibrated
 * Шаг 3: offset[i] = global_mean - averages[i] (для calibrate=true)
 * Шаг 4: offset[i] = 0 (для calibrate=false, не участвуют)
 *
 * Пример:
 *   Датчик A: avg = 78.5°C
 *   Датчик B: avg = 78.9°C
 *   Датчик C: avg = 78.2°C
 *   global_mean = (78.5 + 78.9 + 78.2) / 3 = 78.53°C
 *   offset[A] = 78.53 - 78.5 = +0.03
 *   offset[B] = 78.53 - 78.9 = -0.37
 *   offset[C] = 78.53 - 78.2 = +0.33
 *
 * После калибровки все три датчика будут показывать ~78.53°C.
 */

#include "calibration.h"
#include <Arduino.h>
#include <cmath>

// ============================================================================
// Singleton
// ============================================================================

CalibrationManager& CalibrationManager::getInstance() {
    static CalibrationManager instance;
    return instance;
}

// ============================================================================
// computeOffsets
// ============================================================================

void CalibrationManager::computeOffsets(SensorData* sensors, const float* averages, uint8_t count) {
    if (!sensors || !averages || count == 0) {
        return;
    }

    // Шаг 1: Считаем глобальное среднее только по калиброванным датчикам
    float sum = 0.0f;
    uint8_t cal_count = 0;

    for (uint8_t i = 0; i < count; i++) {
        if (sensors[i].calibrate && averages[i] != 0.0f) {
            sum += averages[i];
            cal_count++;
        }
    }

    if (cal_count == 0) {
        // Нет датчиков для калибровки
        calibrated_count_ = 0;
        return;
    }

    float global_mean = sum / cal_count;

    // Шаг 2: Вычисляем смещения
    calibrated_count_ = 0;
    for (uint8_t i = 0; i < count; i++) {
        if (sensors[i].calibrate && averages[i] != 0.0f) {
            offsets_[i] = global_mean - averages[i];
            sensors[i].offset = offsets_[i];
            calibrated_count_++;

            // Логируем результат
            Serial.printf("[CALIB] Sensor %d (%s): avg=%.3f, offset=%.3f, corrected=%.3f\n",
                          i,
                          sensors[i].address_hex,
                          averages[i],
                          offsets_[i],
                          averages[i] + offsets_[i]);
        } else {
            offsets_[i] = 0.0f;
            sensors[i].offset = 0.0f;
        }
    }

    Serial.printf("[CALIB] Global mean: %.3f°C, calibrated: %d/%d sensors\n",
                  global_mean, calibrated_count_, count);
}

// ============================================================================
// getOffset
// ============================================================================

float CalibrationManager::getOffset(uint8_t index) const {
    if (index >= MAX_SENSORS) {
        return 0.0f;
    }
    return offsets_[index];
}

// ============================================================================
// computeGlobalMean
// ============================================================================

float CalibrationManager::computeGlobalMean(const SensorData* sensors, uint8_t count) const {
    if (!sensors || count == 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    uint8_t valid_count = 0;

    for (uint8_t i = 0; i < count; i++) {
        if (sensors[i].present && sensors[i].temp_corrected != 0.0f) {
            sum += sensors[i].temp_corrected;
            valid_count++;
        }
    }

    if (valid_count == 0) {
        return 0.0f;
    }

    return sum / valid_count;
}
