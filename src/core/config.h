/**
 * @file config.h
 * @brief Глобальные типы, перечисления и константы системы.
 *
 * Этот файл определяет все enum, struct и constexpr-константы,
 * используемые во всех модулях проекта. Центральное место для
 * согласования типов данных между компонентами.
 *
 * Ограничения встраиваемой системы:
 * - Максимум 4 датчика DS18B20 на одной шине OneWire
 * - Размеры буферов подобраны под ESP32-S3 с 8 МБ PSRAM
 * - Все строки фиксированной длины для избежания динамического выделения
 * - dt для PID измеряется через millis(), не захардкожен
 *
 * Два режима работы:
 * - DISTILLATION  — простой перегон, 2 датчика (пар + конденсат),
 *                   два PID: нагрев куба + охлаждение (вентилятор/серво)
 * - RECTIFICATION — колонна с отбором фракций, 3+ датчиков,
 *                   фазы по ΔT, подрежимы vapor/liquid
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <cstring>

// ============================================================================
// Константы системы
// ============================================================================

/** Максимальное количество датчиков DS18B20 на одной шине OneWire */
static constexpr uint8_t  MAX_SENSORS          = 4;
/** Максимальная длина адреса датчика (Dallas/Maxim 64-bit ROM + CRC) */
static constexpr uint8_t  SENSOR_ADDR_LEN       = 8;
/** Максимальная длина строки роли датчика */
static constexpr uint8_t  MAX_ROLE_LEN         = 32;
/** Максимальная длина строки имени датчика */
static constexpr uint8_t  MAX_NAME_LEN         = 32;
/** Максимальная длина MAC-адреса (строковое представление) */
static constexpr uint8_t  MAX_MAC_LEN         = 18;
/** Максимальная длина пути в файловой системе */
static constexpr uint8_t  MAX_PATH_LEN        = 64;
/** Максимальная длина MQTT-темы */
static constexpr uint8_t  MAX_TOPIC_LEN       = 64;
/** Максимальная длина MQTT-пароля/логина */
static constexpr uint8_t  MAX_CRED_LEN        = 64;
/** Максимальная длина имени устройства */
static constexpr uint8_t  MAX_DEV_NAME_LEN    = 32;
/** Максимальная длина WiFi SSID */
static constexpr uint8_t  CFG_MAX_SSID_LEN    = 32;
/** Максимальная длина WiFi пароля */
static constexpr uint8_t  CFG_MAX_WIFI_PASS_LEN = 64;
/** Максимальная длина MQTT хоста */
static constexpr uint8_t  MAX_MQTT_HOST_LEN   = 64;

/** Интервал опроса датчиков DS18B20 (мс). 12-bit конвертация = 750 мс, берём с запасом */
static constexpr uint32_t SENSOR_POLL_MS      = 1000;
/** Количество отсчётов для калибровки (усреднение) */
static constexpr uint8_t  CALIBRATION_SAMPLES  = 20;
/** Интервал между отсчётами при калибровке (мс) */
static constexpr uint16_t CALIBRATION_INTERVAL = 200;

/** Интервал логирования на диск (мс). Батчинг в PSRAM, сброс раз в N секунд */
static constexpr uint32_t LOG_FLUSH_MS         = 5000;
/** Размер кольцевого буфера логов (записей) в PSRAM */
static constexpr uint16_t LOG_BUFFER_SIZE      = 512;
/** Максимальный размер одного логового файла (МБ) */
static constexpr uint8_t  LOG_ROTATION_MB      = 2;
/** Максимальное количество логовых файлов */
static constexpr uint8_t  LOG_MAX_FILES        = 10;

/** Интервал отправки телеметрии через WebSocket (мс) */
static constexpr uint32_t WS_TELEMETRY_MS     = 500;
/** Таймаут ожидания ACK от газового блока (мс) */
static constexpr uint16_t ESPNOW_ACK_TIMEOUT  = 500;
/** Максимальное количество повторных отправок ESP-NOW */
static constexpr uint8_t  ESPNOW_MAX_RETRIES  = 3;

/** Порт HTTP-сервера */
static constexpr uint16_t HTTP_PORT           = 80;
/** Порт WebSocket */
static constexpr uint16_t WS_PORT             = 81;

// ============================================================================
// Перечисления
// ============================================================================

/**
 * @brief Основной режим работы.
 *
 * DISTILLATION  — простой перегон (спирт-сырец), 2 PID-контура
 * RECTIFICATION — колонна с отбором фракций, фазы по ΔT
 */
enum class OperationMode : uint8_t {
    DISTILLATION  = 0,  ///< Простой перегон (distillation)
    RECTIFICATION = 1   ///< Ректификационная колонна
};

/**
 * @brief Подрежим ректификации.
 *
 * VAPOR  — отбор по пару (регулировка через дефлегматор)
 * LIQUID — отбор жидкой фракции (жидкостный кран/клапан)
 */
enum class RectSubMode : uint8_t {
    VAPOR  = 0,  ///< Отбор по пару
    LIQUID = 1   ///< Отбор жидкой фракции
};

/**
 * @brief Режим перехода между фазами ректификации.
 *
 * MANUAL       — ручной переход (кнопкой)
 * DELTA_TEMP   — автоматически по дельте температуры
 * QUANTITATIVE — автоматически по объёму отбора
 */
enum class TransitionMode : uint8_t {
    MANUAL      = 0,  ///< Ручной переход
    DELTA_TEMP  = 1,  ///< По дельте T
    QUANTITATIVE = 2   ///< По объёму
};

/**
 * @brief Тип исполнительного устройства охлаждения.
 *
 * FAN   — вентилятор на радиаторе (PWM → скорость вращения)
 * SERVO — сервопривод на кране (PWM → угол открытия)
 */
enum class CoolerType : uint8_t {
    FAN   = 0,  ///< Вентилятор (PWM → скорость)
    SERVO = 1   ///< Сервопривод (PWM → угол)
};

/**
 * @brief Режим управления газовым блоком (ESP-NOW).
 *
 * AUTO         — контроллер шлёт старт/стоп + мощность, блок сам управляет
 * REMOTE_PID   — контроллер шлёт PID-коэффициенты, блок вычисляет PID
 * MANUAL_POWER — контроллер шлёт фиксированную мощность (ручной режим)
 */
enum class GasControlMode : uint8_t {
    AUTO         = 0,  ///< Авто: старт/стоп + power
    REMOTE_PID   = 1,  ///< Remote PID: шлём kp, ki, kd на блок
    MANUAL_POWER = 2   ///< Ручной: фиксированная мощность
};

/**
 * @brief Фазы процесса ректификации.
 *
 * Конечный автомат переходит между фазами на основе ΔT между
 * датчиками head_selection и body_selection.
 * В режиме DISTILLATION фазы не используются (только IDLE/HEATING).
 */
enum class DistillPhase : uint8_t {
    IDLE      = 0,   ///< Ожидание запуска, нагрев выключен
    HEATING   = 1,   ///< Разгон (до рабочей температуры)
    HEADS     = 2,   ///< Отбор головных фракций (rectification only)
    BODY      = 3,   ///< Отбор тела (основной продукт, rectification only)
    TAILS     = 4,   ///< Отбор хвостовых фракций (rectification only)
    FINISH    = 5,   ///< Процесс завершён
    STATE_ERROR = 6  ///< Аварийное состояние — требуется ручной сброс
};

/**
 * @brief Тип нагревателя.
 *
 * ELECTRIC — прямой ШИМ/реле на GPIO (SSR → ТЭН).
 * GAS      — управление через ESP-NOW на внешний блок.
 */
enum class HeaterType : uint8_t {
    ELECTRIC = 0,
    GAS      = 1
};

/**
 * @brief Тип TFT-контроллера.
 *
 * Поддерживаются популярные контроллеры для ESP32.
 * NONE означает отключённый дисплей.
 */
enum class TFTController : uint8_t {
    NONE   = 0,
    ILI9341 = 1,
    ST7789  = 2,
    ILI9488 = 3
};

/**
 * @brief Роли датчиков на колонне/кубе.
 *
 * column_top       — верх колонны (общая температура, rectification)
 * head_selection   — узел отбора головных фракций (rectification)
 * body_selection   — узел отбора тела, 30 см ниже head_selection (rectification)
 * cooler           — температура охлаждающей жидкости/конденсата
 * boiler           — температура пара в кубе (distillation)
 * custom           — пользовательская роль (для будущих расширений)
 */
enum class SensorRole : uint8_t {
    column_top       = 0,
    head_selection   = 1,
    body_selection   = 2,
    cooler           = 3,
    boiler           = 4,   ///< Температура пара в кубе (distillation)
    custom           = 5    ///< Произвольная роль
};

/**
 * @brief Тип команды для WebSocket / API.
 */
enum class CommandType : uint8_t {
    SET_POWER        = 0,  ///< Установка мощности нагрева
    SET_PID_CUBE     = 1,  ///< Настройка PID куба
    SET_PID_COOLER   = 2,  ///< Настройка PID охладителя
    SET_PHASE        = 3,  ///< Ручная установка фазы
    START_PHASE      = 4,  ///< Автозапуск фазы
    EMERGENCY_STOP   = 5,  ///< Аварийная остановка
    RESET_ERROR      = 6,  ///< Сброс аварии
    CALIBRATE        = 7,  ///< Запуск калибровки
    SET_THRESHOLD    = 8,  ///< Установка порогов ΔT
    AI_COMMAND       = 9,  ///< Команда от ИИ-агента
    SET_MODE         = 10, ///< Переключение режима (distillation/rectification)
    SET_SUB_MODE     = 11, ///< Подрежим (vapor/liquid)
    SET_COOLER_TYPE  = 12, ///< Тип охладителя (fan/servo)
    SET_SENSOR_ROLE  = 13, ///< Назначение роли датчику
    SET_SENSOR_ACTIVE = 14 ///< Активация/деактивация датчика
};

// ============================================================================
// Структуры данных
// ============================================================================

/**
 * @brief Данные одного датчика температуры.
 *
 * Хранит сырые и скорректированные значения, а также
 * параметры калибровки. Каждый датчик может быть
 * активным или неактивным (временно исключён из расчётов).
 */
struct SensorData {
    char     address_hex[SENSOR_ADDR_LEN * 2 + 1]; ///< HEX-строка адреса
    SensorRole role;                               ///< Роль датчика
    char     name[MAX_NAME_LEN];                   ///< Человекочитаемое имя
    float    temp_raw;                             ///< Сырое значение (без коррекции)
    float    temp_corrected;                       ///< Скорректированное значение
    float    offset;                               ///< Смещение (калибровка)
    bool     calibrate;                            ///< Флаг автоматической калибровки
    float    manual_offset;                        ///< Ручное смещение (если calibrate=false)
    bool     present;                              ///< Датчик обнаружен на шине
    bool     active;                               ///< Участвует ли в расчётах (UI on/off)
    uint32_t last_update_ms;                       ///< Время последнего обновления (millis)
};

/**
 * @brief Полный снапшот состояния системы.
 *
 * Используется для WebSocket-телеметрии, MQTT-публикаций
 * и snapshot-API. Все поля актуальны на момент вызова.
 */
struct SystemSnapshot {
    OperationMode mode;                            ///< Текущий режим (distillation/rectification)
    RectSubMode  rect_sub_mode;                    ///< Подрежим ректификации
    DistillPhase phase;                            ///< Текущая фаза
    SensorData   sensors[MAX_SENSORS];              ///< Данные всех датчиков
    uint8_t      sensor_count;                     ///< Количество обнаруженных датчиков
    HeaterType   heater_type;                      ///< Тип нагревателя
    uint8_t      heater_power;                     ///< Текущая мощность нагрева (0-100%)
    float        pid_out_cube;                     ///< Выход PID куба (%)
    float        pid_out_cooler;                   ///< Выход PID охладителя (%)
    float        delta_t;                          ///< ΔT = T_select - T_below (rectification)
    float        dist_target_temp;                 ///< Целевая температура (distillation)
    float        dist_current_temp;                ///< Текущая температура пара (distillation)
    uint32_t     uptime_sec;                       ///< Время работы (секунды)
    uint8_t      error_count;                      ///< Количество активных ошибок
    bool         heater_enabled;                   ///< Разрешение нагрева
    bool         calibrating;                      ///< Идёт ли калибровка
    uint32_t     timestamp_ms;                     ///< Метка времени (millis)
};

/**
 * @brief Данные управления газовым нагревателем (ESP-NOW).
 *
 * Передаются от контроллера к газовому блоку.
 * Содержит уставку мощности и флаг разрешения нагрева.
 */
struct GasControlData {
    uint8_t  power;          ///< Мощность 0-100%
    bool     enable;         ///< Разрешение нагрева
    uint32_t crc;            ///< CRC32 для целостности данных
} __attribute__((packed));

/**
 * @brief PID-конфиг для газового блока (ESP-NOW, remote PID режим).
 *
 * Используется когда контроллер делегирует PID-регулирование
 * внешнему газовому блоку. Блок получает коэффициенты и
 * вычисляет PID самостоятельно.
 */
struct GasPIDConfig {
    float    kp;             ///< Пропорциональный коэффициент
    float    ki;             ///< Интегральный коэффициент
    float    kd;             ///< Дифференциальный коэффициент
    float    setpoint;       ///< Целевая температура
    uint8_t  mode;           ///< GasControlMode: AUTO/REMOTE_PID/MANUAL
    uint32_t crc;            ///< CRC32 для целостности данных
} __attribute__((packed));

/**
 * @brief Ответ газового блока (ESP-NOW).
 *
 * Подтверждение получения команды и статус блока.
 */
struct GasStatusData {
    bool     ack;            ///< Подтверждение получения
    uint8_t  status;         ///< Статус газового блока (0=OK, >0=ошибка)
    float    gas_temp;       ///< Температура газового блока (если есть датчик)
    uint32_t crc;            ///< CRC32 для целостности данных
} __attribute__((packed));

/**
 * @brief Данные от Level & ABV модуля (ESP-NOW канал 2).
 *
 * Модуль измеряет уровень жидкости, крепость, давление
 * и температуру жидкости. Данные передаются по ESP-NOW.
 */
struct LevelModuleData {
    uint16_t level_mm;       ///< Уровень в мм
    float    abv;            ///< Крепость (% об.)
    float    pressure;      ///< Давление (мм рт.ст.)
    float    temp_liquid;   ///< Температура жидкости (°C)
    uint32_t crc;           ///< CRC32 для целостности
} __attribute__((packed));

/**
 * @brief Команда контроллера -> Level модуль.
 */
struct LevelModuleCmd {
    uint8_t  mode;          ///< 0=запрос данных, 1=калибровка
    uint16_t reserved;     ///< Зарезервировано
    uint32_t crc;           ///< CRC32
} __attribute__((packed));

#define MAX_CALIBRATION_POINTS 16

/**
 * @brief Запись калибровки Level модуля (mm -> ml).
 */
struct LevelCalibrationPoint {
    uint16_t mm;   ///< Уровень в мм
    uint16_t ml;   ///< Объём в мл
} __attribute__((packed));

/**
 * @brief Состояние приёма емкости (Collection Tank).
 */
struct CollectionTankState {
    uint16_t current_ml;      ///< Текущий объём в мл
    uint16_t session_ml;       ///< Суммарный объём за сессию
    uint16_t fill_percent;     ///< Процент заполнения
    bool    warning_sent;      ///< Предупреждение отправлено
    bool    stopped;           ///< Остановлен по заполнении
    uint32_t last_update_ts;   ///< Timestamp последнего обновления
};

/**
 * @brief Параметры PID-регулятора.
 *
 * Классическая формула: output = Kp*e + Ki*∫e*dt + Kd*de/dt
 * dt измеряется через millis() при каждом вызове.
 */
struct PIDParams {
    float kp;
    float ki;
    float kd;
    float out_min;
    float out_max;
    float integral_max;
};

/**
 * @brief Конфигурация системы (полная структура из settings.json).
 */
struct SystemConfig {
    // --- system ---
    char          device_name[MAX_DEV_NAME_LEN];
    char          wifi_ssid[CFG_MAX_SSID_LEN];
    char          wifi_pass[CFG_MAX_WIFI_PASS_LEN];
    OperationMode mode;           ///< DISTILLATION или RECTIFICATION

    // --- sensors ---
    uint8_t  one_wire_pin;       ///< Пин OneWire для DS18B20
    SensorData sensors[MAX_SENSORS];
    uint8_t  sensor_count;

    // --- heater ---
    HeaterType      heater_type;
    uint8_t         pin_pwm;            ///< Пин ШИМ для электрического нагревателя
    uint8_t         pin_cooler;         ///< Пин ШИМ для охладителя
    uint8_t         espnow_mac_gas[6];  ///< MAC-адрес газового блока (байты)
    CoolerType      cooler_type;        ///< FAN или SERVO (дистилляция)
    GasControlMode  gas_control_mode;   ///< AUTO / REMOTE_PID / MANUAL_POWER

    // --- rectification ---
    RectSubMode     rect_sub_mode;      ///< VAPOR или LIQUID
    TransitionMode transition_mode;    ///< MANUAL / DELTA_TEMP / QUANTITATIVE
    float           threshold_heads_end;
    float           threshold_body_end;
    float           delta_hysteresis;

    // --- distillation ---
    float           dist_target_temp;       ///< Целевая температура пара
    float           dist_temp_tolerance;    ///< Допуск температуры (±°C)
    float           quantitative_heads_ml;  ///< Volume (ml) to transition from HEADS to BODY
    float           quantitative_body_ml;   ///< Volume (ml) to transition from BODY to TAILS

    // --- PID куба (нагрев) ---
    PIDParams pid_cube;

    // --- PID охладителя (rectification — дефлегматор) ---
    PIDParams pid_cooler;

    // --- PID охладителя (distillation — конденсатор) ---
    PIDParams pid_dist_cooler;

    // --- mqtt ---
    bool     mqtt_enabled;
    char     mqtt_host[MAX_MQTT_HOST_LEN];
    uint16_t mqtt_port;
    char     mqtt_user[MAX_CRED_LEN];
    char     mqtt_pass[MAX_CRED_LEN];
    char     mqtt_topic_root[MAX_TOPIC_LEN];

    // --- logging ---
    char     log_path[MAX_PATH_LEN];
    uint8_t  log_max_files;
    uint8_t  log_rotation_mb;

    // --- tft ---
    bool         tft_enabled;
    TFTController tft_controller;
    uint8_t      tft_spi_clk;
    uint8_t      tft_spi_mosi;
    uint8_t      tft_spi_miso;
    uint8_t      tft_spi_cs;
    uint8_t      tft_spi_dc;
    uint8_t      tft_spi_rst;
    uint8_t      tft_backlight;
};

// ============================================================================
// Вспомогательные функции
// ============================================================================

/** @brief Преобразование OperationMode в строку */
inline const char* modeToString(OperationMode mode) {
    return (mode == OperationMode::DISTILLATION) ? "distillation" : "rectification";
}

/** @brief Преобразование строки в OperationMode */
inline OperationMode stringToMode(const char* str) {
    if (str == nullptr) return OperationMode::RECTIFICATION;
    if (strcmp(str, "distillation") == 0) return OperationMode::DISTILLATION;
    return OperationMode::RECTIFICATION;
}

/** @brief Преобразование RectSubMode в строку */
inline const char* subModeToString(RectSubMode mode) {
    return (mode == RectSubMode::VAPOR) ? "vapor" : "liquid";
}

/** @brief Преобразование строки в RectSubMode */
inline RectSubMode stringToSubMode(const char* str) {
    if (str == nullptr) return RectSubMode::VAPOR;
    if (strcmp(str, "liquid") == 0) return RectSubMode::LIQUID;
    return RectSubMode::VAPOR;
}

/** @brief Преобразование CoolerType в строку */
inline const char* coolerTypeToString(CoolerType type) {
    return (type == CoolerType::FAN) ? "fan" : "servo";
}

/** @brief Преобразование строки в CoolerType */
inline CoolerType stringToCoolerType(const char* str) {
    if (str == nullptr) return CoolerType::FAN;
    if (strcmp(str, "servo") == 0) return CoolerType::SERVO;
    return CoolerType::FAN;
}

/** @brief Преобразование GasControlMode в строку */
inline const char* gasControlModeToString(GasControlMode mode) {
    switch (mode) {
        case GasControlMode::AUTO:         return "auto";
        case GasControlMode::REMOTE_PID:   return "remote_pid";
        case GasControlMode::MANUAL_POWER: return "manual";
        default:                           return "unknown";
    }
}

/** @brief Преобразование строки в GasControlMode */
inline GasControlMode stringToGasControlMode(const char* str) {
    if (str == nullptr) return GasControlMode::AUTO;
    if (strcmp(str, "remote_pid") == 0) return GasControlMode::REMOTE_PID;
    if (strcmp(str, "manual") == 0) return GasControlMode::MANUAL_POWER;
    return GasControlMode::AUTO;
}

/** @brief Преобразование DistillPhase в строку (для логов и JSON) */
inline const char* phaseToString(DistillPhase phase) {
    switch (phase) {
        case DistillPhase::IDLE:       return "idle";
        case DistillPhase::HEATING:    return "heating";
        case DistillPhase::HEADS:      return "heads";
        case DistillPhase::BODY:       return "body";
        case DistillPhase::TAILS:      return "tails";
        case DistillPhase::FINISH:     return "finish";
        case DistillPhase::STATE_ERROR:return "error";
        default:                       return "unknown";
    }
}

/** @brief Преобразование строки в DistillPhase */
inline DistillPhase stringToPhase(const char* str) {
    if (str == nullptr) return DistillPhase::IDLE;
    if (__builtin_strcmp(str, "idle") == 0)       return DistillPhase::IDLE;
    if (__builtin_strcmp(str, "heating") == 0)    return DistillPhase::HEATING;
    if (__builtin_strcmp(str, "heads") == 0)      return DistillPhase::HEADS;
    if (__builtin_strcmp(str, "body") == 0)       return DistillPhase::BODY;
    if (__builtin_strcmp(str, "tails") == 0)      return DistillPhase::TAILS;
    if (__builtin_strcmp(str, "finish") == 0)     return DistillPhase::FINISH;
    if (__builtin_strcmp(str, "error") == 0)      return DistillPhase::STATE_ERROR;
    return DistillPhase::IDLE;
}

/** @brief Преобразование SensorRole в строку */
inline const char* roleToString(SensorRole role) {
    switch (role) {
        case SensorRole::column_top:      return "column_top";
        case SensorRole::head_selection:  return "head_selection";
        case SensorRole::body_selection:  return "body_selection";
        case SensorRole::cooler:          return "cooler";
        case SensorRole::boiler:          return "boiler";
        case SensorRole::custom:          return "custom";
        default:                          return "unknown";
    }
}

/** @brief Преобразование строки в SensorRole */
inline SensorRole stringToRole(const char* str) {
    if (str == nullptr) return SensorRole::cooler;
    if (strcmp(str, "column_top") == 0)     return SensorRole::column_top;
    if (strcmp(str, "head_selection") == 0) return SensorRole::head_selection;
    if (strcmp(str, "body_selection") == 0) return SensorRole::body_selection;
    if (strcmp(str, "cooler") == 0)         return SensorRole::cooler;
    if (strcmp(str, "boiler") == 0)         return SensorRole::boiler;
    if (strcmp(str, "custom") == 0)         return SensorRole::custom;
    return SensorRole::cooler;
}

/** @brief Преобразование HeaterType в строку */
inline const char* heaterTypeToString(HeaterType type) {
    return (type == HeaterType::ELECTRIC) ? "electric" : "gas";
}

/** @brief Преобразование TFTController в строку */
inline const char* tftControllerToString(TFTController ctrl) {
    switch (ctrl) {
        case TFTController::ILI9341: return "ILI9341";
        case TFTController::ST7789:  return "ST7789";
        case TFTController::ILI9488: return "ILI9488";
        default:                  return "NONE";
    }
}

/** @brief Преобразование строки в TFTController */
inline TFTController stringToTFTController(const char* str) {
    if (str == nullptr) return TFTController::NONE;
    if (strcmp(str, "ILI9341") == 0) return TFTController::ILI9341;
    if (strcmp(str, "ST7789") == 0)  return TFTController::ST7789;
    if (strcmp(str, "ILI9488") == 0) return TFTController::ILI9488;
    return TFTController::NONE;
}

#endif // CONFIG_H
