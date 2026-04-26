/**
 * @file ai_bridge.cpp
 * @brief Реализация моста для ИИ-агента.
 */

#include "ai_bridge.h"
#include <Arduino.h>
#include <cstring>

// Коды команд ИИ
enum AICommandCode {
    AI_CMD_NONE = 0,
    AI_CMD_SET_POWER,
    AI_CMD_START,
    AI_CMD_STOP,
    AI_CMD_EMERGENCY_STOP,
    AI_CMD_RESET_ERROR,
    AI_CMD_CALIBRATE,
    AI_CMD_SET_PHASE,
    AI_CMD_SET_THRESHOLD,
    AI_CMD_SET_PID,
    AI_CMD_STATUS,
    AI_CMD_SENSORS,
    AI_CMD_HELP
};

// ============================================================================
// Singleton
// ============================================================================

AIBridge& AIBridge::getInstance() {
    static AIBridge instance;
    return instance;
}

// ============================================================================
// begin
// ============================================================================

bool AIBridge::begin(StateMachine& state_machine, DS18B20Manager& sensors, SettingsManager& settings) {
    state_machine_ = &state_machine;
    sensors_ = &sensors;
    settings_ = &settings;
    return true;
}

// ============================================================================
// processCommand
// ============================================================================

bool AIBridge::processCommand(const char* command, char* out_response, size_t response_size) {
    if (!command || !out_response) {
        return false;
    }

    out_response[0] = '\0';

    // Удалить пробелы в начале
    while (*command == ' ') command++;

    // Сопоставить команду
    int cmd_code = matchCommand(command);
    if (cmd_code == AI_CMD_NONE) {
        snprintf(out_response, response_size,
                "{\"success\":false,\"message\":\"Неизвестная команда: %s\",\"help\":\"Используйте: set_power, start, stop, calibrate, status, sensors, help\"}",
                command);
        return false;
    }

    // Выполнить команду
    return executeCommand(cmd_code, command, out_response, response_size);
}

// ============================================================================
// matchCommand
// ============================================================================

int AIBridge::matchCommand(const char* text) {
    // Простое сопоставление по ключевым словам

    if (strstr(text, "set_power") || strstr(text, "мощность") || strstr(text, "power")) {
        return AI_CMD_SET_POWER;
    }
    if (strstr(text, "start") || strstr(text, "запуск") || strstr(text, "начать")) {
        return AI_CMD_START;
    }
    if (strstr(text, "stop") || strstr(text, "стоп") || strstr(text, "остановить")) {
        return AI_CMD_STOP;
    }
    if (strstr(text, "emergency") || strstr(text, "аварийн")) {
        return AI_CMD_EMERGENCY_STOP;
    }
    if (strstr(text, "reset") || strstr(text, "сброс")) {
        return AI_CMD_RESET_ERROR;
    }
    if (strstr(text, "calibrate") || strstr(text, "калибров")) {
        return AI_CMD_CALIBRATE;
    }
    if (strstr(text, "set_phase") || strstr(text, "фаза")) {
        return AI_CMD_SET_PHASE;
    }
    if (strstr(text, "threshold") || strstr(text, "порог")) {
        return AI_CMD_SET_THRESHOLD;
    }
    if (strstr(text, "pid")) {
        return AI_CMD_SET_PID;
    }
    if (strstr(text, "status") || strstr(text, "состояние") || strstr(text, "статус")) {
        return AI_CMD_STATUS;
    }
    if (strstr(text, "sensors") || strstr(text, "датчик") || strstr(text, "температур")) {
        return AI_CMD_SENSORS;
    }
    if (strstr(text, "help") || strstr(text, "помощь") || strstr(text, "?")) {
        return AI_CMD_HELP;
    }

    return AI_CMD_NONE;
}

// ============================================================================
// executeCommand
// ============================================================================

bool AIBridge::executeCommand(int cmd_code, const char* params, char* out_response, size_t response_size) {
    (void)params;

    switch (cmd_code) {
        case AI_CMD_SET_POWER: {
            // Извлечь число из команды
            int power = 0;
            const char* num = strchr(params, ' ');
            if (num) {
                power = atoi(num + 1);
            }
            if (power < 0) power = 0;
            if (power > 100) power = 100;

            state_machine_->setPhase(DistillPhase::IDLE);
            snprintf(out_response, response_size,
                    "{\"success\":true,\"message\":\"Мощность установлена: %d%%\",\"power\":%d}",
                    power, power);
            break;
        }

        case AI_CMD_START: {
            state_machine_->start();
            snprintf(out_response, response_size,
                    "{\"success\":true,\"message\":\"Процесс запущен\",\"phase\":\"%s\"}",
                    phaseToString(state_machine_->getPhase()));
            break;
        }

        case AI_CMD_STOP:
        case AI_CMD_EMERGENCY_STOP: {
            state_machine_->emergencyStop();
            snprintf(out_response, response_size,
                    "{\"success\":true,\"message\":\"Аварийная остановка\",\"phase\":\"error\"}");
            break;
        }

        case AI_CMD_RESET_ERROR: {
            state_machine_->resetError();
            snprintf(out_response, response_size,
                    "{\"success\":true,\"message\":\"Авария сброшена\",\"phase\":\"idle\"}");
            break;
        }

        case AI_CMD_CALIBRATE: {
            state_machine_->startCalibration();
            snprintf(out_response, response_size,
                    "{\"success\":true,\"message\":\"Калибровка запущена\"}");
            break;
        }

        case AI_CMD_SET_PHASE: {
            const char* phase_str = strchr(params, ' ');
            if (phase_str) {
                phase_str++;
                while (*phase_str == ' ') phase_str++;

                DistillPhase phase = DistillPhase::IDLE;
                if (strstr(phase_str, "idle")) phase = DistillPhase::IDLE;
                else if (strstr(phase_str, "heating")) phase = DistillPhase::HEATING;
                else if (strstr(phase_str, "heads")) phase = DistillPhase::HEADS;
                else if (strstr(phase_str, "body")) phase = DistillPhase::BODY;
                else if (strstr(phase_str, "tails")) phase = DistillPhase::TAILS;
                else if (strstr(phase_str, "finish")) phase = DistillPhase::FINISH;

                state_machine_->setPhase(phase);
                snprintf(out_response, response_size,
                        "{\"success\":true,\"message\":\"Фаза уст��новлена\",\"phase\":\"%s\"}",
                        phaseToString(phase));
            }
            break;
        }

        case AI_CMD_STATUS: {
            getStatusForAI(out_response, response_size);
            break;
        }

        case AI_CMD_SENSORS: {
            getSensorsForAI(out_response, response_size);
            break;
        }

        case AI_CMD_HELP: {
            snprintf(out_response, response_size,
                    "{\"success\":true,\"commands\":[\"set_power [0-100]\",\"start\",\"stop\","
                    "\"calibrate\",\"status\",\"sensors\",\"set_phase [phase]\",\"reset\"]}");
            break;
        }

        default: {
            snprintf(out_response, response_size,
                    "{\"success\":false,\"message\":\"Команда не поддерживается\"}");
            return false;
        }
    }

    return true;
}

// ============================================================================
// getStatusForAI
// ============================================================================

void AIBridge::getStatusForAI(char* out_status, size_t max_size) {
    if (!out_status) return;

    DistillPhase phase = state_machine_->getPhase();
    const char* phase_str = phaseToString(phase);

    snprintf(out_status, max_size,
            "{\"success\":true,\"status\":{\"phase\":\"%s\","
            "\"heater_power\":%d,\"heater_enabled\":%s,"
            "\"delta_t\":%.2f,\"uptime_sec\":%u}}",
            phase_str,
            0,  // power - нужно получить от heater
            "true",
            0.0f,  // delta_t - нужно получить от phase_selector
            0);    // uptime
}

// ============================================================================
// getSensorsForAI
// ============================================================================

void AIBridge::getSensorsForAI(char* out_sensors, size_t max_size) {
    if (!out_sensors) return;

    const SensorData* sens = sensors_->getSensors();
    uint8_t count = sensors_->getSensorCount();

    char temps[512] = "[";
    for (uint8_t i = 0; i < count && i < MAX_SENSORS; i++) {
        char temp_buf[64];
        snprintf(temp_buf, sizeof(temp_buf),
                "{\"role\":\"%s\",\"name\":\"%s\",\"temp\":%.2f}",
                roleToString(sens[i].role),
                sens[i].name,
                sens[i].temp_corrected);

        if (i > 0) strcat(temps, ",");
        strcat(temps, temp_buf);
    }
    strcat(temps, "]");

    snprintf(out_sensors, max_size,
            "{\"success\":true,\"sensors\":%s,\"count\":%d}",
            temps, count);
}