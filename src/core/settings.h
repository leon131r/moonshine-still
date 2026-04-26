/**
 * @file settings.h
 * @brief Менеджер настроек: загрузка/сохранение settings.json, валидация, watcher.
 *
 * Модуль отвечает за:
 * - Чтение конфигурации из LittleFS (settings.json)
 * - Валидацию структуры и применение дефолтов при ошибках
 * - Сериализацию обратно в JSON (при сохранении калибровки, порогов)
 * - Отслеживание изменений файла (watcher по mtime) — перечитка без перезагрузки
 *
 * Ограничения встраиваемой системы:
 * - JSON-документ размещается в PSRAM (динамический, ~4KB)
 * - При отсутствии PSRAM fallback на heap
 * - Файловый watcher опрашивает mtime каждые 5 секунд
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "config.h"
#include <Arduino.h>

/**
 * @brief Класс управления настройками системы.
 *
 * Singleton-паттерн: один экземпляр на всю систему.
 * Все данные хранятся в RAM (SystemConfig), синхронизация с FS по требованию.
 */
class SettingsManager {
public:
    /**
     * @brief Получить единственный экземпляр (singleton).
     * @return Ссылка на SettingsManager
     */
    static SettingsManager& getInstance();

    /**
     * @brief Инициализация менеджера.
     * @param fs_path Путь к settings.json (например, "/settings.json")
     * @return true при успешной загрузке, false — применены дефолты
     *
     * Вызывает load() из файла. Если файл не найден или невалиден —
     * заполняет config дефолтными значениями и сохраняет.
     */
    bool begin(const char* fs_path);

    /**
     * @brief Загрузить настройки из файла.
     * @return true при успехе, false при ошибке чтения/парсинга
     */
    bool load();

    /**
     * @brief Сохранить текущие настройки в файл.
     * @return true при успехе, false при ошибке записи
     *
     * Вызывается при изменении калибровки, порогов, параметров PID.
     * Полная перезапись файла.
     */
    bool save();

    /**
     * @brief Проверить изменения файла (watcher).
     *
     * Сравнивает текущий mtime файла с сохранённым.
     * Если файл изменён — вызывает load() автоматически.
     * Вызывать из loop() каждые ~5 секунд.
     */
    void watch();

    /** @brief Получить копию текущей конфигурации */
    const SystemConfig& getConfig() const { return config_; }

/** @brief Получить изменяемую ссылку на конфигурацию */
    SystemConfig& getConfigMutable() { return config_; }
    
    /** @brief Установить режим работы */
    bool setMode(OperationMode mode);
    
    /**
     * @brief Обновить настройки газового блока.
     * @param mac MAC-адрес (6 байт)
     */
    void setGasMAC(const uint8_t mac[6]);

    /**
     * @brief Сохранить смещения калибровки датчиков.
     * @param offsets Массив смещений (по индексу в sensors[])
     * @param count   Количество датчиков
     * @return true при успехе
     *
     * Вызывается модулем calibration после расчёта offset.
     * Сохраняет offsets в sensor[].offset и пишет settings.json на диск.
     */
    bool saveCalibrationOffsets(const float* offsets, uint8_t count);

    /**
     * @brief Обновить пороги фаз и сохранить.
     * @param heads_end   Порог окончания голов
     * @param body_end    Порог окончания тела
     * @param hysteresis  Гистерезис
     * @return true при успехе
     */
bool savePhaseThresholds(float heads_end, float body_end, float hysteresis);

    /**
     * @brief Обновить параметры PID куба и сохранить.
     */
    bool savePIDCubeParams(const PIDParams& params);

    /**
     * @brief Обновить параметры PID охладителя (ректификация) и сохранить.
     */
    bool savePIDCoolerParams(const PIDParams& params);

    /**
     * @brief Обновить параметры PID охладителя (дистилляция) и сохранить.
     */
    bool savePIDDistCoolerParams(const PIDParams& params);

    /** @brief true, если watcher обнаружил изменения */
    bool hasChanged() const { return changed_; }

    /** @brief Сбросить флаг изменений */
    void clearChanged() { changed_ = false; }

    /**
     * @brief Сброс к заводским настройкам (публичный).
     */
    void resetToDefaults() { applyDefaults(); save(); }

private:
    void applyDefaults();

    /**
     * @brief Валидация загруженной конфигурации.
     * @return true если конфигурация корректна
     *
     * Проверяет:
     * - Диапазоны порогов (heads_end < body_end)
     * - PIN-ы в допустимых диапазонах
     * - Длины строк
     * - MQTT port > 0
     */
    bool validate();

    SystemConfig config_;          ///< Текущая конфигурация в RAM
    char         file_path_[64];   ///< Путь к settings.json
    uint32_t     file_mtime_ = 0;  ///< Последний известный mtime файла
    bool         changed_ = false; ///< Флаг: файл был изменён (watcher)
};

#endif // SETTINGS_H
