/**
 * @file logger.h
 * @brief Структурированное NDJSON-логирование на LittleFS.
 *
 * Архитектура:
 * 1. Записи накапливаются в кольцевом буфере (PSRAM)
 * 2. При заполнении буфера или по таймеру — сброс на диск
 * 3. Авто-ротация: при достижении лимита файла создаётся новый
 * 4. Старые файлы удаляются при превышении max_files
 *
 * Формат строки:
 *   {"ts":1712345678,"phase":"body","T_top":78.2,"T_select":78.5,
 *    "T_below":78.9,"pid_out":45.2,"heater_type":"electric","errors":0}
 *
 * Использование PSRAM:
 * - Кольцевой буфер LOG_BUFFER_SIZE записей (~512 × 256 байт = 128 КБ)
 * - Минимизация записей на Flash (продлевает жизнь памяти)
 * - При отсутствии PSRAM fallback на heap (меньший буфер)
 *
 * API для выгрузки: getLastEntries() — последние N записей из буфера.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include "core/config.h"
#include "core/settings.h"
#include <Arduino.h>

/**
 * @brief Кольцевой буфер логов.
 *
 * Реализован как циклический массив в PSRAM.
 * При переполнении старые записи перезаписываются.
 */
struct LogRingBuffer {
    char** entries;           ///< Массив указателей на строки (PSRAM)
    uint16_t size;            ///< Максимальный размер
    uint16_t head;            ///< Позиция записи
    uint16_t count;           ///< Количество записей в буфере
    bool     full;            ///< Буфер заполнен (перезапись)
};

/**
 * @brief Класс логирования.
 *
 * Singleton. Неблокирующий: запись в буфер — мгновенная,
 * сброс на диск — по таймеру из poll().
 */
class Logger {
public:
    /** @brief Получить экземпляр (singleton) */
    static Logger& getInstance();

    /**
     * @brief Инициализация.
     * @param settings Ссылка на менеджер настроек
     * @return true при успехе
     *
     * Выделяет буфер в PSRAM, создаёт директорию логов.
     */
    bool begin(SettingsManager& settings);

    /**
     * @brief Добавить запись в лог.
     * @param snapshot Текущий снапшот системы
     *
     * НЕ блокирует. Сериализует snapshot в NDJSON-строку
     * и добавляет в кольцевой буфер.
     */
    void log(const SystemSnapshot& snapshot);

    /**
     * @brief Добавить произвольную NDJSON-строку.
     * @param json_str JSON-строка (одна строка)
     *
     * Используется для системных сообщений (ошибки, события).
     */
    void logRaw(const char* json_str);

    /**
     * @brief Сбросить буфер на диск.
     * @return true при успехе
     *
     * Вызывается автоматически из poll() по таймеру
     * или вручную через API.
     */
    bool flush();

    /**
     * @brief Неблокирующий опрос.
     *
     * Вызывать из loop(). Проверяет:
     * - Таймер сброса буфера на диск
     * - Необходимость ротации файлов
     */
    void poll();

    /**
     * @brief Получить последние N записей из буфера.
     * @param out_buf    Буфер для результатов (NDJSON, по строке на запись)
     * @param buf_size   Размер буфера
     * @param max_entries Максимум записей
     * @return Количество возвращённых записей
     *
     * Используется API /api/log/latest.
     */
    uint16_t getLastEntries(char* out_buf, size_t buf_size, uint16_t max_entries);

    /** @brief Количество записей в буфере */
    uint16_t getBufferedCount() const;

    /** @brief Имя текущего логового файла */
    const char* getCurrentFileName() const { return current_file_; }

    /** @brief Общее количество записей (всех файлов) */
    uint32_t getTotalLogged() const { return total_logged_; }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * @brief Выделить кольцевой буфер.
     * @return true при успехе
     *
     * Приоритет: PSRAM (ps_malloc) → heap (malloc).
     */
    bool allocateBuffer();

    /** Освободить буфер */
    void freeBuffer();

    /**
     * @ Проверить необходимость ротации.
     * @return true если файл был ротирован
     *
     * Ротация при:
     * - Размер файла > rotation_mb
     * - Количество файлов > max_files (удаляется самый старый)
     */
    bool checkRotation();

    /**
     * @brief Создать новый логовый файл.
     * @return true при успехе
     *
     * Имя: /log/log_YYYYMMDD_HHMMSS.ndjson
     */
    bool openNewFile();

    /**
     * @brief Сериализовать снапшот в NDJSON-строку.
     * @param snapshot Снапшот
     * @param buf      Буфер назначения
     * @param buf_size Размер буфера
     * @return Длина строки
     */
    size_t snapshotToJSON(const SystemSnapshot& snapshot, char* buf, size_t buf_size);

    SettingsManager* settings_ = nullptr;
    LogRingBuffer    ring_buf_;

    char    current_file_[64];   ///< Имя текущего файла
    uint32_t flush_timer_ = 0;   ///< Таймер сброса (millis)
    uint32_t total_logged_ = 0;  ///< Общее количество записей
    bool     initialized_ = false;
};

#endif // LOGGER_H
