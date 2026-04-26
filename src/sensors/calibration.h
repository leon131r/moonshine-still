/**
 * @file calibration.h
 * @brief Менеджер калибровки датчиков температуры.
 *
 * Алгоритм калибровки:
 * 1. Все датчики с calibrate=true находятся в одинаковых условиях
 *    (погружены в одну среду).
 * 2. Собирается N отсчётов, вычисляется среднее для каждого: T_avg[i]
 * 3. Вычисляется общее среднее: T_global = mean(T_avg[i] для всех i)
 * 4. Смещение для каждого датчика: offset[i] = T_global - T_avg[i]
 * 5. При чтении: T_corrected = T_raw + offset[i]
 *
 * Это гарантирует, что все калиброванные датчики показывают
 * одинаковую температуру при нахождении в одной среде.
 *
 * Смещения сохраняются в settings.json и применяются автоматически.
 */

#ifndef CALIBRATION_H
#define CALIBRATION_H

#include "core/config.h"

/**
 * @brief Класс управления калибровкой.
 *
 * Singleton. Вызывается из DS18B20Manager после сбора отсчётов.
 * Не зависит от файловой системы — только вычисления.
 * Сохранение через SettingsManager.
 */
class CalibrationManager {
public:
    /** @brief Получить экземпляр (singleton) */
    static CalibrationManager& getInstance();

    /**
     * @brief Вычислить смещения датчиков.
     * @param sensors      Массив SensorData (из DS18B20Manager)
     * @param averages     Средние температуры для каждого датчика
     * @param count        Количество датчиков
     *
     * Алгоритм:
     * 1. Найти среднее всех averages (только для calibrate=true)
     * 2. Для каждого: offset = global_mean - average[i]
     * 3. Записать offset в sensors[i].offset
     */
    void computeOffsets(SensorData* sensors, const float* averages, uint8_t count);

    /**
     * @brief Получить смещение конкретного датчика.
     * @param index Индекс в массиве
     * @return Смещение (°C), 0 если не калиброван
     */
    float getOffset(uint8_t index) const;

    /** @brief Количество калиброванных датчиков */
    uint8_t getCalibratedCount() const { return calibrated_count_; }

    /** @brief true, если калибровка выполнена хотя бы для одного датчика */
    bool isCalibrated() const { return calibrated_count_ > 0; }

    /**
     * @brief Рассчитать среднюю температуру всех калиброванных датчиков.
     * @param sensors Массив SensorData
     * @param count   Количество
     * @return Средняя температура (°C) или 0 если нет калиброванных
     *
     * Используется для верификации: после калибровки все corrected
     * температуры должны быть близки к этому значению.
     */
    float computeGlobalMean(const SensorData* sensors, uint8_t count) const;

private:
    CalibrationManager() = default;
    CalibrationManager(const CalibrationManager&) = delete;
    CalibrationManager& operator=(const CalibrationManager&) = delete;

    float    offsets_[MAX_SENSORS];    ///< Вычисленные смещения
    uint8_t  calibrated_count_ = 0;   ///< Количество калиброванных
};

#endif // CALIBRATION_H
