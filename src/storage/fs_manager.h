/**
 * @file fs_manager.h
 * @brief Менеджер файловой системы LittleFS.
 *
 * Отвечает за:
 * - Инициализацию и монтирование LittleFS
 * - Автоматическое форматирование при повреждении FS
 * - Базовые операции чтения/записи файлов
 *
 * LittleFS на ESP32-S3 использует ~1-2 МБ Flash.
 * Раздел определяется в default_16MB.csv.
 */

#ifndef FS_MANAGER_H
#define FS_MANAGER_H

#include <Arduino.h>
#include <LittleFS.h>

/**
 * @brief Класс управления LittleFS.
 *
 * Singleton. Все файловые операции проходят через этот класс.
 */
class FSManager {
public:
    /** @brief Получить экземпляр (singleton) */
    static FSManager& getInstance();

    /**
     * @brief Инициализация LittleFS.
     * @param max_open_files Максимум одновременно открытых файлов (default 5)
     * @return true при успехе, false — при ошибке форматирования
     *
     * Пытается смонтировать FS. Если не удалось — форматирует.
     * Вызывать ОДИН РАЗ в setup(), до любых файловых операций.
     */
    bool begin(uint8_t max_open_files = 5);

    /**
     * @brief Прочитать файл в буфер.
     * @param path   Путь к файлу
     * @param buf    Буфер назначения (должен быть достаточного размера)
     * @param buf_size Размер буфера
     * @return Количество прочитанных байт, 0 при ошибке
     *
     * Буфер должен быть выделен вызывающим кодом.
     * Функция не делает динамического выделения памяти.
     */
    size_t readFile(const char* path, char* buf, size_t buf_size);

    /**
     * @brief Записать данные в файл.
     * @param path Путь к файлу
     * @param data Данные для записи
     * @param len  Размер данных
     * @return true при успехе
     *
     * Полная перезапись файла. Создаёт файл если не существует.
     */
    bool writeFile(const char* path, const char* data, size_t len);

    /**
     * @brief Записать данные в файл (String-перегрузка).
     */
    bool writeFile(const char* path, const String& data);

    /**
     * @brief Добавить данные в конец файла (append).
     * @param path Путь к файлу
     * @param data Данные
     * @param len  Размер
     * @return true при успехе
     *
     * Используется логгером для батчинга записей.
     */
    bool appendFile(const char* path, const char* data, size_t len);

    /** @brief Проверить существование файла */
    bool exists(const char* path) const;

    /** @brief Удалить файл */
    bool removeFile(const char* path);

    /** @brief Получить размер файла (байт), 0 при ошибке */
    size_t fileSize(const char* path);

    /** @brief Получить свободное место на FS (байт) */
    size_t freeSpace() const;

    /** @brief Получить общий размер FS (байт) */
    size_t totalSpace() const;

    /**
     * @brief Создать директорию (рекурсивно).
     * @param path Путь директории
     * @return true при успехе
     *
     * LittleFS не поддерживает вложенные mkdir,
     * поэтому функция создаёт каждый уровень отдельно.
     */
    bool mkdir(const char* path);

    /**
     * @brief Получить список файлов в директории.
     * @param dir_path   Путь директории
     * @param out_buf    Буфер для результатов (формат: "file1\nfile2\n...")
     * @param buf_size   Размер буфера
     * @return Количество найденных файлов
     */
    uint8_t listDir(const char* dir_path, char* out_buf, size_t buf_size);

    /**
     * @brief Открыть файл для чтения/записи.
     * @param path Путь к файлу
     * @param mode Режим открытия ("r", "w", "a")
     * @return File объект (проверять на !f для ошибки)
     *
     * Возвращает File объект напрямую из LittleFS.
     * Вызывающий код должен вызвать f.close().
     */
    File openFile(const char* path, const char* mode);

    /**
     * @brief Открыть директорию для чтения.
     * @param path Путь к директории
     * @return File объект директории (проверять на !d для ошибки)
     */
    File openDir(const char* path);

    /**
     * @brief Получить использованное место на FS (байт)
     */
    size_t usedSpace() const;

private:
    FSManager() = default;
    FSManager(const FSManager&) = delete;
    FSManager& operator=(const FSManager&) = delete;

    bool mounted_ = false;
};

#endif // FS_MANAGER_H
