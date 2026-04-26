/**
 * @file collection_tank.h
 * @brief Логика приёма емкости (Collection Tank).
 *
 * Пересчитывает level_mm в volume_ml по таблице калибровки.
 * Отслеживает заполнение, скорость отбора, суммарный объём.
 */

#ifndef COLLECTION_TANK_H
#define COLLECTION_TANK_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/config.h"

#define COLLECTION_TANK_MAX_POINTS 16

class CollectionTank {
public:
    CollectionTank();

    void begin(const LevelCalibrationPoint* table, uint8_t count);

    void updateLevel(uint16_t level_mm);

    void updateVolume(uint16_t volume_ml);

    bool isFull() const { return state_.fill_percent >= 100; }

    bool shouldWarn() const { return state_.fill_percent >= warning_percent_ && !state_.warning_sent; }

    uint16_t getCurrentMl() const { return state_.current_ml; }

    uint16_t getSessionMl() const { return state_.session_ml; }

    uint16_t getFillPercent() const { return state_.fill_percent; }

    float getFillRateMlPerMin() const { return fill_rate_ml_per_min_; }

    void setCapacity(uint16_t ml) { capacity_ml_ = ml; updateFillPercent(); }

    void setWarningPercent(uint8_t pct) { warning_percent_ = pct; }

    void stopCollection();
    void resumeCollection();
    bool isStopped() const { return state_.stopped; }

void continueCollection();

    void setBaseline(uint16_t mm) { baseline_mm_ = mm; baseline_set_time_ = millis(); Serial.printf("[COLTANK] Baseline set: %dmm\n", mm); }
    uint16_t getBaseline() const { return baseline_mm_; }
    bool isBaselineReady() const { return (millis() - baseline_set_time_) > 2000; }

    void reset();

    void getStateForTelemetry(JsonObject& doc) const;

private:
    void updateFillPercent();
    uint16_t interpolateMl(uint16_t level_mm) const;

    LevelCalibrationPoint calibration_[COLLECTION_TANK_MAX_POINTS];
    uint8_t calibration_count_;
    uint16_t capacity_ml_;
    uint8_t warning_percent_;
    CollectionTankState state_;
    float fill_rate_ml_per_min_;
    uint32_t last_rate_update_;
    uint16_t last_level_ml_;
    uint16_t baseline_mm_ = 0;
    uint32_t baseline_set_time_ = 0;
};

#endif // COLLECTION_TANK_H