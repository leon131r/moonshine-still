/**
 * @file containers.h
 * @brief Менеджер ёмкостей: хранение, CRUD, выбор.
 */

#ifndef CONTAINERS_H
#define CONTAINERS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/config.h"

#define CONTAINERS_MAX_COUNT 16
#define CONTAINERS_FILE "/containers.json"

struct ContainerInfo {
    uint8_t  id;
    char     name[32];
    uint16_t volume_ml;
    uint16_t diameter_mm;
};

class ContainersManager {
public:
    static ContainersManager& getInstance();

    bool begin();

    uint8_t count() const { return count_; }

    const ContainerInfo* getById(uint8_t id) const;
    const ContainerInfo* getSelected() const { return getById(selected_id_); }

    uint8_t getSelectedId() const { return selected_id_; }

    bool add(const char* name, uint16_t volume_ml, uint16_t diameter_mm);

    bool update(uint8_t id, const char* name, uint16_t volume_ml, uint16_t diameter_mm);

    bool remove(uint8_t id);

    bool select(uint8_t id);

    bool save();

    bool load();

    void getAsJSON(JsonArray& arr) const;

private:
    ContainersManager() = default;
    ContainersManager(const ContainersManager&) = delete;
    ContainersManager& operator=(const ContainersManager&) = delete;

    uint8_t nextId();

    ContainerInfo containers_[CONTAINERS_MAX_COUNT];
    uint8_t count_ = 0;
    uint8_t selected_id_ = 0;
};

#endif // CONTAINERS_H