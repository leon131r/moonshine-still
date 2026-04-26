/**
 * @file containers.cpp
 * @brief Реализация ContainersManager.
 */

#include "storage/containers.h"
#include "storage/fs_manager.h"
#include <LittleFS.h>

ContainersManager& ContainersManager::getInstance() {
    static ContainersManager instance;
    return instance;
}

bool ContainersManager::begin() {
    return load();
}

bool ContainersManager::load() {
    if (!FSManager::getInstance().exists(CONTAINERS_FILE)) {
        Serial.println("[CONTAINERS] File not found, creating defaults");
        selected_id_ = 0;
        count_ = 0;
        return save();
    }

    File f = LittleFS.open(CONTAINERS_FILE, "r");
    if (!f) {
        Serial.println("[CONTAINERS] Failed to open file");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[CONTAINERS] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonArray arr = doc["containers"].as<JsonArray>();
    count_ = 0;
    for (JsonObject c : arr) {
        if (count_ >= CONTAINERS_MAX_COUNT) break;
        ContainerInfo& ci = containers_[count_];
        ci.id = c["id"] | (count_ + 1);
        strncpy(ci.name, c["name"] | "", 31);
        ci.name[31] = '\0';
        ci.volume_ml = c["volume_ml"] | 0;
        ci.diameter_mm = c["diameter_mm"] | 0;
        count_++;
    }

    selected_id_ = doc["selected_id"] | 0;

    Serial.printf("[CONTAINERS] Loaded %d containers, selected=%d\n", count_, selected_id_);
    return true;
}

bool ContainersManager::save() {
    File f = LittleFS.open(CONTAINERS_FILE, "w");
    if (!f) {
        Serial.println("[CONTAINERS] Failed to open file for writing");
        return false;
    }

    JsonDocument doc;
    JsonArray arr = doc["containers"].to<JsonArray>();
    for (uint8_t i = 0; i < count_; i++) {
        JsonObject c = arr.add<JsonObject>();
        c["id"] = containers_[i].id;
        c["name"] = containers_[i].name;
        c["volume_ml"] = containers_[i].volume_ml;
        c["diameter_mm"] = containers_[i].diameter_mm;
    }
    doc["selected_id"] = selected_id_;

    if (serializeJson(doc, f) == 0) {
        Serial.println("[CONTAINERS] serializeJson failed");
        f.close();
        return false;
    }

    f.close();
    Serial.printf("[CONTAINERS] Saved %d containers\n", count_);
    return true;
}

uint8_t ContainersManager::nextId() {
    uint8_t max_id = 0;
    for (uint8_t i = 0; i < count_; i++) {
        if (containers_[i].id > max_id) max_id = containers_[i].id;
    }
    return max_id + 1;
}

const ContainerInfo* ContainersManager::getById(uint8_t id) const {
    for (uint8_t i = 0; i < count_; i++) {
        if (containers_[i].id == id) return &containers_[i];
    }
    return nullptr;
}

bool ContainersManager::add(const char* name, uint16_t volume_ml, uint16_t diameter_mm) {
    if (count_ >= CONTAINERS_MAX_COUNT) {
        Serial.println("[CONTAINERS] Max count reached");
        return false;
    }

    ContainerInfo& ci = containers_[count_];
    ci.id = nextId();
    strncpy(ci.name, name, 31);
    ci.name[31] = '\0';
    ci.volume_ml = volume_ml;
    ci.diameter_mm = diameter_mm;
    count_++;

    if (selected_id_ == 0) {
        selected_id_ = ci.id;
    }

    Serial.printf("[CONTAINERS] Added: id=%d, name='%s', volume=%d, diameter=%d\n",
                  ci.id, ci.name, ci.volume_ml, ci.diameter_mm);

    return save();
}

bool ContainersManager::update(uint8_t id, const char* name, uint16_t volume_ml, uint16_t diameter_mm) {
    for (uint8_t i = 0; i < count_; i++) {
        if (containers_[i].id == id) {
            strncpy(containers_[i].name, name, 31);
            containers_[i].name[31] = '\0';
            containers_[i].volume_ml = volume_ml;
            containers_[i].diameter_mm = diameter_mm;
            Serial.printf("[CONTAINERS] Updated id=%d\n", id);
            return save();
        }
    }
    return false;
}

bool ContainersManager::remove(uint8_t id) {
    for (uint8_t i = 0; i < count_; i++) {
        if (containers_[i].id == id) {
            for (uint8_t j = i; j < count_ - 1; j++) {
                containers_[j] = containers_[j + 1];
            }
            count_--;

            if (selected_id_ == id) {
                selected_id_ = (count_ > 0) ? containers_[0].id : 0;
            }

            Serial.printf("[CONTAINERS] Removed id=%d\n", id);
            return save();
        }
    }
    return false;
}

bool ContainersManager::select(uint8_t id) {
    if (id == 0) {
        selected_id_ = 0;
        return true;
    }

    for (uint8_t i = 0; i < count_; i++) {
        if (containers_[i].id == id) {
            selected_id_ = id;
            Serial.printf("[CONTAINERS] Selected id=%d\n", id);
            return true;
        }
    }
    return false;
}

void ContainersManager::getAsJSON(JsonArray& arr) const {
    for (uint8_t i = 0; i < count_; i++) {
        JsonObject c = arr.add<JsonObject>();
        c["id"] = containers_[i].id;
        c["name"] = containers_[i].name;
        c["volume_ml"] = containers_[i].volume_ml;
        c["diameter_mm"] = containers_[i].diameter_mm;
        c["selected"] = (containers_[i].id == selected_id_);
    }
}