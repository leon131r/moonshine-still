/**
 * @file ds18b20_manager.h
 * @brief Менеджер датчиков DS18B20 (OneWire).
 *
 * Модуль отвечает за:
 * - Сканирование шины OneWire при старте
 * - Сопоставление обнаруженных устройств с конфигом (по адресу)
 * - Неблокирующий асинхронный опрос всех датчиков
 * - Температурные данные с коррекцией (через calibration)
 *
 * Ограничения встраиваемой системы:
 * - Максимум 4 датчика на одной шине (ограничение config.h)
 * - 12-bit разрешение = 750 мс конвертация
 * - Опрос через millis() — НЕ блокирует loop()
 * - Все датчики на ОДНОМ GPIO (один объект OneWire)
 */

#ifndef DS18B20_MANAGER_H
#define DS18B20_MANAGER_H

#include "core/config.h"
#include "core/settings.h"
#include <OneWire.h>
#include <DallasTemperature.h>

/**
 * @brief Класс управления датчиками DS18B20.
 *
 * Singleton. Неблокирующий опрос через конечный автомат:
 * IDLE → REQUEST → WAIT → READ → IDLE
 */
class DS18B20Manager {
public:
    /** @brief Получить экземпляр (singleton) */
    static DS18B20Manager& getInstance();

    /**
     * @brief Инициализация менеджера датчиков.
     * @param one_wire_pin  GPIO пин шины OneWire
     * @param settings      Ссылка на менеджер настроек
     * @return true при успехе
     *
     * Последовательность:
     * 1. Инициализация OneWire на указанном пине
     * 2. Привязка DallasTemperature
     * 3. Сканирование шины
     * 4. Сопоставление с конфигом (по address_hex)
     * 5. Установка разрешения (12-bit для всех)
     */
    bool begin(uint8_t one_wire_pin, SettingsManager& settings);

    /**
     * @brief Неблокирующий опрос датчиков.
     *
     * Вызывать из loop() КАЖДЫЙ цикл.
     * Машина состояний:
     * - REQUEST: отправка команды requestTemperatures()
     * - WAIT: ожидание конвертации (750 мс для 12-bit)
     * - READ: считывание температур, применение коррекции
     *
     * НЕ блокирует выполнение. Использует millis() для таймингов.
     */
    void poll();

    /**
     * @brief Получить массив данных датчиков.
     * @return Указатель на массив SensorData
     */
    const SensorData* getSensors() const { return sensors_; }

    /** @brief Количество обнаруженных датчиков */
    uint8_t getSensorCount() const { return detected_count_; }

    /** @brief Количество датчиков в конфиге */
    uint8_t getConfigSensorCount() const;

    /** @brief Внутренние обнаруженные адреса */
    uint8_t getDetectedCount() const { return detected_count_; }

    /**
     * @brief Получить данные конкретного датчика по роли.
     * @param role Роль датчика
     * @return Указатель на SensorData или nullptr если не найден
     */
    const SensorData* getSensorByRole(SensorRole role) const;

    /**
     * @brief Запуск ручной калибровки.
     *
     * Собирает N отсчётов с датчиков, вычисляет среднее.
     * Результат передаётся в CalibrationManager.
     * @return true если калибровка запущена
     */
    bool startCalibration();

    /** @brief Идёт ли сейчас калибровка */
    bool isCalibrating() const { return calibrating_; }

    /** @brief Прогресс калибровки (0-100%) */
    uint8_t getCalibrationProgress() const;

    /**
     * @brief Получить температуру конкретного датчика.
     * @param index Индекс в массиве (0..detected_count_-1)
     * @return Температура (°C), или DEVICE_DISCONNECTED_RAW при ошибке
     */
    float getTemp(uint8_t index) const;

    /** @brief true, если все датчики опрошены успешно */
    bool allSensorsValid() const;

    /**
     * @brief Пересканировать датчики (публичный).
     */
    void rescan() { scanBus(); matchWithConfig(); }

private:
    DS18B20Manager() = default;
    DS18B20Manager(const DS18B20Manager&) = delete;
    DS18B20Manager& operator=(const DS18B20Manager&) = delete;

    /**
     * @brief Сканирование шины OneWire.
     * @return Количество обнаруженных устройств
     *
     * Использует OneWire::search() для поиска всех устройств на шине.
     * Результат записывается в detected_addrs_[].
     */
    uint8_t scanBus();

    /**
     * @brief Сопоставить обнаруженные устройства с конфигом.
     *
     * Сравнивает address_hex из конфига с реально обнаруженными.
     * Заполняет sensors_[] данными: адрес, роль, имя, calibrate.
     */
    void matchWithConfig();

    /**
     * @brief Применить коррекцию температуры.
     * @param index Индекс датчика
     * @return Скорректированная температура
     *
     * Формула:
     * - Если calibrate:   T_corrected = T_raw + offset
     * - Иначе:            T_corrected = T_raw + manual_offset
     */
    float applyCorrection(uint8_t index, float raw_temp) const;

    /**
     * @brief Неблокирующий опрос калибровки.
     */
    void pollCalibration();

    /**
     * @brief Преобразовать 8-байтовый адрес в HEX-строку.
     */
    static void addressToHex(const uint8_t* addr, char* hex_buf, size_t buf_size);

    // Состояние опроса
    enum class PollState : uint8_t {
        IDLE,        ///< Ожидание
        REQUEST,     ///< Отправка requestTemperatures()
        WAITING,     ///< Ожидание конвертации
        READING      ///< Чтение температур
    };

    OneWire*             one_wire_ = nullptr;          ///< Шина OneWire
    DallasTemperature*   dallas_ = nullptr;            ///< Библиотека Dallas
    SettingsManager*     settings_ = nullptr;          ///< Ссылка на настройки

    SensorData           sensors_[MAX_SENSORS];        ///< Данные датчиков
    uint8_t              detected_count_ = 0;          ///< Количество обнаруженных
    uint8_t              detected_addrs_[MAX_SENSORS][8]; ///< Адреса обнаруженных

    PollState            poll_state_ = PollState::IDLE; ///< Состояние опроса
    uint32_t             poll_timer_ = 0;              ///< Таймер опроса (millis)

    // Калибровка
    bool                 calibrating_ = false;         ///< Флаг калибровки
    uint8_t              calib_samples_ = 0;           ///< Собрано отсчётов
    float                calib_sum_[MAX_SENSORS];      ///< Сумма температур (для усреднения)
    uint32_t             calib_timer_ = 0;             ///< Таймер между отсчётами
};

#endif // DS18B20_MANAGER_H
