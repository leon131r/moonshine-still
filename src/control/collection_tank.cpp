/**
 * @file collection_tank.cpp
 * @brief Реализация CollectionTank.
 */

#include "collection_tank.h"

CollectionTank::CollectionTank()
    : calibration_count_(0)
    , capacity_ml_(2000)
    , warning_percent_(90)
    , fill_rate_ml_per_min_(0.0f)
    , last_rate_update_(0)
    , last_level_ml_(0)
{
    memset(&state_, 0, sizeof(state_));
}

void CollectionTank::begin(const LevelCalibrationPoint* table, uint8_t count) {
    calibration_count_ = (count < COLLECTION_TANK_MAX_POINTS) ? count : COLLECTION_TANK_MAX_POINTS;
    for (uint8_t i = 0; i < calibration_count_; i++) {
        calibration_[i] = table[i];
    }
    Serial.printf("[COLTANK] Loaded %d calibration points, capacity=%dml\n",
                  calibration_count_, capacity_ml_);
}

void CollectionTank::updateLevel(uint16_t level_mm) {
    Serial.printf("[COLTANK] updateLevel: raw=%d, baseline=%d, ready=%d, elapsed=%lu\n", 
                  level_mm, baseline_mm_, isBaselineReady(), millis() - baseline_set_time_);
    if (!isBaselineReady()) {
        Serial.println("[COLTANK] Baseline settling, ignoring level");
        return;
    }
    uint16_t effective_mm = (level_mm > baseline_mm_) ? (level_mm - baseline_mm_) : 0;
    Serial.printf("[COLTANK] effective_mm=%d\n", effective_mm);
    uint16_t ml = interpolateMl(effective_mm);
    Serial.printf("[COLTANK] ml=%d\n", ml);
    updateVolume(ml);
}

void CollectionTank::updateVolume(uint16_t volume_ml) {
    uint32_t now = millis();

    // Расчёт скорости
    if (last_rate_update_ > 0) {
        uint32_t dt_ms = now - last_rate_update_;
        if (dt_ms > 0) {
            int32_t delta_ml = (int32_t)volume_ml - (int32_t)last_level_ml_;
            if (delta_ml > 0) {
                float dt_min = dt_ms / 60000.0f;
                fill_rate_ml_per_min_ = delta_ml / dt_min;
            }
        }
    }

    state_.current_ml = volume_ml;
    state_.session_ml += (volume_ml > last_level_ml_) ? (volume_ml - last_level_ml_) : 0;
    state_.last_update_ts = now / 1000;
    last_level_ml_ = volume_ml;
    last_rate_update_ = now;

    updateFillPercent();

    if (shouldWarn()) {
        state_.warning_sent = true;
        Serial.printf("[COLTANK] Warning: %d%% full (%dml)\n",
                      state_.fill_percent, state_.current_ml);
    }

    if (state_.fill_percent >= 100 && !state_.stopped) {
        state_.stopped = true;
        Serial.println("[COLTANK] Tank full, stopped");
    }
}

void CollectionTank::continueCollection() {
    state_.current_ml = 0;
    state_.warning_sent = false;
    state_.stopped = false;
    last_level_ml_ = 0;
}

void CollectionTank::stopCollection() {
    state_.stopped = true;
    Serial.println("[COLTANK] Collection stopped manually");
}

void CollectionTank::resumeCollection() {
    state_.stopped = false;
    state_.warning_sent = false;
    Serial.println("[COLTANK] Collection resumed");
}

void CollectionTank::reset() {
    memset(&state_, 0, sizeof(state_));
    fill_rate_ml_per_min_ = 0;
    last_rate_update_ = 0;
    last_level_ml_ = 0;
    Serial.println("[COLTANK] Reset");
}

void CollectionTank::getStateForTelemetry(JsonObject& doc) const {
    doc["current_ml"] = state_.current_ml;
    doc["session_ml"] = state_.session_ml;
    doc["fill_percent"] = state_.fill_percent;
    doc["fill_rate"] = fill_rate_ml_per_min_;
    doc["capacity_ml"] = capacity_ml_;
    doc["warning"] = state_.warning_sent && !state_.stopped;
    doc["stopped"] = state_.stopped;
}

void CollectionTank::updateFillPercent() {
    if (capacity_ml_ > 0) {
        state_.fill_percent = min(100, (state_.current_ml * 100) / capacity_ml_);
    }
}

uint16_t CollectionTank::interpolateMl(uint16_t level_mm) const {
    if (calibration_count_ == 0) return 0;
    if (calibration_count_ == 1) return calibration_[0].ml;

    // Нижняя граница
    if (level_mm <= calibration_[0].mm) {
        return calibration_[0].ml;
    }
    // Верхняя граница
    if (level_mm >= calibration_[calibration_count_ - 1].mm) {
        return calibration_[calibration_count_ - 1].ml;
    }

    // Линейная интерполяция
    for (uint8_t i = 0; i < calibration_count_ - 1; i++) {
        if (level_mm >= calibration_[i].mm && level_mm < calibration_[i + 1].mm) {
            uint16_t x1 = calibration_[i].mm;
            uint16_t x2 = calibration_[i + 1].mm;
            uint16_t y1 = calibration_[i].ml;
            uint16_t y2 = calibration_[i + 1].ml;

            float ratio = (float)(level_mm - x1) / (float)(x2 - x1);
            return y1 + (uint16_t)(ratio * (y2 - y1));
        }
    }

    return 0;
}