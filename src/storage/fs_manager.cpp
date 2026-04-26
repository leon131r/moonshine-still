/**
 * @file fs_manager.cpp
 * @brief Реализация менеджера LittleFS.
 *
 * Все операции проверяют наличие смонтированной FS.
 * При ошибке чтения/записи — логирование через Serial
 * (в продакшене заменить на logger после его инициализации).
 */

#include "fs_manager.h"
#include <LittleFS.h>

// ============================================================================
// Singleton
// ============================================================================

FSManager& FSManager::getInstance() {
    static FSManager instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool FSManager::begin(uint8_t max_open_files) {
    if (mounted_) {
        return true; // Уже смонтирована
    }

    // Пробуем смонтировать
    if (!LittleFS.begin(true)) {
        // Не удалось смонтировать — форматируем
        Serial.println("[FS] LittleFS mount failed, formatting...");
        LittleFS.format();

        // Пробуем ещё раз
        if (!LittleFS.begin(true)) {
            Serial.println("[FS] ERROR: LittleFS format failed!");
            return false;
        }
    }

    mounted_ = true;
    Serial.printf("[FS] LittleFS mounted. Total: %u bytes, Free: %u bytes\n",
                  LittleFS.totalBytes(), LittleFS.totalBytes() - LittleFS.usedBytes());
    return true;
}

// ============================================================================
// readFile
// ============================================================================

size_t FSManager::readFile(const char* path, char* buf, size_t buf_size) {
    if (!mounted_ || !path || !buf || buf_size == 0) {
        return 0;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        return 0;
    }

    // Читаем не больше buf_size - 1 (оставляем место для null-терминатора)
    size_t to_read = (f.size() < buf_size - 1) ? f.size() : buf_size - 1;
    size_t read = f.readBytes(buf, to_read);
    buf[read] = '\0';

    f.close();
    return read;
}

// ============================================================================
// writeFile
// ============================================================================

bool FSManager::writeFile(const char* path, const char* data, size_t len) {
    if (!mounted_ || !path || !data) {
        return false;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        return false;
    }

    size_t written = f.write((const uint8_t*)data, len);
    f.close();

    return (written == len);
}

bool FSManager::writeFile(const char* path, const String& data) {
    return writeFile(path, data.c_str(), data.length());
}

// ============================================================================
// appendFile
// ============================================================================

bool FSManager::appendFile(const char* path, const char* data, size_t len) {
    if (!mounted_ || !path || !data) {
        return false;
    }

    File f = LittleFS.open(path, "a");
    if (!f) {
        return false;
    }

    size_t written = f.write((const uint8_t*)data, len);
    f.close();

    return (written == len);
}

// ============================================================================
// exists
// ============================================================================

bool FSManager::exists(const char* path) const {
    if (!mounted_) return false;
    return LittleFS.exists(path);
}

// ============================================================================
// removeFile
// ============================================================================

bool FSManager::removeFile(const char* path) {
    if (!mounted_) return false;
    return LittleFS.remove(path);
}

// ============================================================================
// fileSize
// ============================================================================

size_t FSManager::fileSize(const char* path) {
    if (!mounted_) return 0;

    File f = LittleFS.open(path, "r");
    if (!f) return 0;

    size_t sz = f.size();
    f.close();
    return sz;
}

// ============================================================================
// freeSpace / totalSpace
// ============================================================================

size_t FSManager::freeSpace() const {
    if (!mounted_) return 0;
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

size_t FSManager::totalSpace() const {
    if (!mounted_) return 0;
    return LittleFS.totalBytes();
}

// ============================================================================
// mkdir
// ============================================================================

bool FSManager::mkdir(const char* path) {
    if (!mounted_) return false;
    return LittleFS.mkdir(path);
}

// ============================================================================
// listDir
// ============================================================================

uint8_t FSManager::listDir(const char* dir_path, char* out_buf, size_t buf_size) {
    if (!mounted_ || !out_buf || buf_size == 0) {
        return 0;
    }

    out_buf[0] = '\0';
    size_t pos = 0;
    uint8_t count = 0;

    File root = LittleFS.open(dir_path);
    if (!root || !root.isDirectory()) {
        return 0;
    }

    File f = root.openNextFile();
    while (f && count < 64) { // Ограничение: максимум 64 файлов
        const char* name = f.name();
        size_t name_len = strlen(name);

        // Проверяем, влезет ли имя + newline
        if (pos + name_len + 2 > buf_size) {
            break; // Буфер заполнен
        }

        strcpy(out_buf + pos, name);
        pos += name_len;
        out_buf[pos++] = '\n';
        out_buf[pos] = '\0';

        count++;
        f.close();
        f = root.openNextFile();
    }

    if (f) f.close();
    return count;
}

// ============================================================================
// openFile
// ============================================================================

File FSManager::openFile(const char* path, const char* mode) {
    if (!mounted_ || !path || !mode) {
        return File();
    }
    return LittleFS.open(path, mode);
}

// ============================================================================
// openDir
// ============================================================================

File FSManager::openDir(const char* path) {
    if (!mounted_ || !path) {
        return File();
    }
    File d = LittleFS.open(path);
    if (d && d.isDirectory()) {
        return d;
    }
    return File();
}

// ============================================================================
// usedSpace
// ============================================================================

size_t FSManager::usedSpace() const {
    if (!mounted_) return 0;
    return LittleFS.usedBytes();
}
