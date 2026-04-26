/**
 * @file ai_bridge.h
 * @brief Мост для ИИ-агента: обработка команд от внешнего ИИ.
 *
 * Модуль отвечает за:
 * - Приём команд от ИИ-агента (WebSocket или MQTT)
 * - Валидацию и парсинг команд
 * - Передачу команд в state_machine
 * - Возврат результата ИИ-агенту
 *
 * Формат команд ИИ:
 * - "set_power 50" → {cmd: "set_power", value: 50}
 * - "start process" → {cmd: "start"}
 * - "stop" → {cmd: "emergency_stop"}
 * - "calibrate sensors" → {cmd: "calibrate"}
 * - "set phase body" → {cmd: "set_phase", phase: "body"}
 * - "query status" → Возвращает текущий статус
 * - "query sensors" → Возвращает показания датчиков
 *
 * Зависимости:
 * - state_machine.h - для выполнения команд
 * - ds18b20_manager.h - для запроса датчиков
 * - settings.h - для конфигурации
 */

#ifndef AI_BRIDGE_H
#define AI_BRIDGE_H

#include "core/config.h"
#include "core/state_machine.h"
#include "sensors/ds18b20_manager.h"
#include "core/settings.h"

/**
 * @brief Класс моста для ИИ-агента.
 *
 * Singleton. Обрабатывает текстовые команды от ИИ,
 * преобразует в структуры системы и возвращает результат.
 */
class AIBridge {
public:
    /**
     * @brief Получить экземпляр (singleton).
     */
    static AIBridge& getInstance();

    /**
     * @brief Инициализация.
     * @param state_machine Ссылка на автомат
     * @param sensors Ссылка на менеджер датчиков
     * @param settings Ссылка на настройки
     * @return true при успехе
     */
    bool begin(StateMachine& state_machine, DS18B20Manager& sensors, SettingsManager& settings);

    /**
     * @brief Обработать команду от ИИ.
     * @param command Текстовая команда
     * @param out_response Ответ для ИИ (заполняется)
     * @return true если команда распознана
     *
     * Примеры:
     * - "set_power 50" → выполнить и вернуть результат
     * - "start" → начать процесс
     * - "status" → вернуть текущее состояние
     */
    bool processCommand(const char* command, char* out_response, size_t response_size);

    /**
     * @brief Получить статус для ИИ.
     * @param out_status JSON статуса (заполняется)
     * @param max_size Размер буфера
     *
     * Возвращает полный JSON состояния системы.
     */
    void getStatusForAI(char* out_status, size_t max_size);

    /**
     * @brief Получить показания датчиков для ИИ.
     * @param out_sensors JSON датчиков (заполняется)
     * @param max_size Размер буфера
     */
    void getSensorsForAI(char* out_sensors, size_t max_size);

private:
    AIBridge() = default;
    AIBridge(const AIBridge&) = delete;
    AIBridge& operator=(const AIBridge&) = delete;

    StateMachine* state_machine_ = nullptr;
    DS18B20Manager* sensors_ = nullptr;
    SettingsManager* settings_ = nullptr;

    /**
     * @brief Сопоставить текстовую команду с командами системы.
     * @return Код команды или -1 если не распознано
     */
    int matchCommand(const char* text);

    /**
     * @brief Выполнить команду.
     */
    bool executeCommand(int cmd_code, const char* params, char* out_response, size_t response_size);
};

#endif // AI_BRIDGE_H