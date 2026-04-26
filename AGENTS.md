# AGENTS.md — Руководство для агентного кодирования

## Обзор

ESP32-S3 контроллер ректификационной колонны. PlatformIO + Arduino. Два режима: DISTILLATION (2 датчика, 2 PID) и RECTIFICATION (3+ датчика, фазы по ΔT).

---

## Команды сборки

```bash
pio run                          # Сборка прошивки (обязательно перед загрузкой/тестом)
pio run --target build --filter "src/main.cpp"  # Сборка одного файла (быстрая проверка)
pio run --target upload          # Загрузка прошивки в ESP32-S3
pio run --target uploadfs        # Загрузка LittleFS (веб-интерфейс)
pio device monitor               # Мониторинг Serial (115200 baud)
pio run --target clean           # Очистка артефактов сборки
```

**Примечание:** Модульные тесты не настроены. Основная проверка — успешная сборка (`pio run`).

---

## Стиль кода

### Защита заголовков
```cpp
#ifndef MODULE_NAME_H
#define MODULE_NAME_H
// ... код ...
#endif // MODULE_NAME_H
```

### Порядок include
1. Заголовки фреймворка (`Arduino.h`, `ArduinoJson.h`)
2. Внешние библиотеки из `lib_deps`
3. Локальные include с `" "` (группировка по директориям)

```cpp
#include <Arduino.h>
#include <ArduinoJson.h>

#include "core/config.h"
#include "sensors/ds18b20_manager.h"
```

### Именование

| Элемент | Стиль | Пример |
|---------|-------|--------|
| Классы | PascalCase | `StateMachine`, `DS18B20Manager` |
| Функции | camelCase | `begin()`, `updateSnapshot()` |
| Переменные | snake_case | `g_start_time_ms`, `heater_power` |
| Константы | SCREAMING_SNAKE | `TELEMETRY_INTERVAL_MS` |
| Значения enum | SCREAMING_SNAKE | `IDLE = 0`, `HEADS = 2` |
| Структуры | PascalCase | `SystemSnapshot`, `PIDParams` |
| Члены класса | snake_case_ | `heater_power_`, `config_` |
| Глобальные переменные | префикс `g_` | `g_snapshot` |

### Типы данных
- Фиксированная ширина: `uint8_t`, `uint16_t`, `uint32_t`
- `float` для температур и PID, `bool` для флагов
- `char[]` для строк фиксированной длины (без динамики)
- `static constexpr` для констант времени компиляции
- `enum class` с указанием типа: `enum class DistillPhase : uint8_t { IDLE = 0, HEATING = 1, HEADS = 2, ... };`

### Обработка ошибок
- Возвращай `bool` для операций (`begin()`, `update()`)
- Логируй с префиксом: `Serial.printf("[Module] Error: %d\n", err);`
- При невосстановимых ошибках — переход в `STATE_ERROR`
- Глобальный счётчик: `g_snapshot.error_count`

### Ограничения памяти
- **Избегай `new`/`malloc`** — используй статические буферы
- `PSRAM` для больших буферов
- Все строковые буферы фиксированного размера `MAX_*_LEN`

---

## Архитектурные паттерны

### Синглтоны
```cpp
static SomeManager& getInstance() {
    static SomeManager instance;
    return instance;
}
```
Вызов: `SomeManager::getInstance().method()`

### Интерфейс IHeater
```cpp
class IHeater {
public:
    virtual void setPower(uint8_t power) = 0;
    virtual uint8_t getPower() const = 0;
    virtual void enableHeating(bool enable) = 0;
    virtual bool isHeatingEnabled() const = 0;
    virtual void emergencyStop() = 0;
    virtual HeaterType getType() const = 0;
};
```

### PID-регулятор
- dt измеряется через `millis()` при каждом вызове
- Параметры PID загружаются из `settings.json`

---

## Структура проекта

```
src/
├── main.cpp              # Точка входа, loop(), setup()
├── core/                 # config.h, settings.h/cpp, state_machine.h/cpp
├── sensors/              # ds18b20_manager.h/cpp, calibration.h/cpp
├── control/              # heater_interface.h, electric_heater, gas_heater, phase_selector
├── comm/                 # web_api, mqtt_handler, espnow_handler, ai_bridge
└── storage/              # fs_manager, logger, containers

data/                    # Веб-UI (LittleFS)
platformio.ini           # Конфигурация сборки
settings.json           # Конфигурация runtime
```

---

## Важные замечания

- **PID dt** измеряется через `millis()` — не захардкоживай интервалы
- **Настройки** из JSON загружаются при запуске
- **Web API**: HTTP порт 80, WebSocket порт 81
- **ESP-NOW**: связь с газовым блоком и модулем уровня (6-байтный MAC)
- **State machine**: фазы блокируются в `STATE_ERROR` — сброс только вручную

### Отладка HTTP

```bash
# Тест endpoint
curl -s -X POST "http://192.168.0.106/api/container/select?id=1"
```

- Пути API **не должны быть префиксами** друг друга (ESPAsyncWebServer конфликт)
- Не вызывай `save()` (LittleFS) внутри HTTP callback — блокирует

---

## Быстрая справка

```cpp
// Доступ к датчику по роли
const SensorData* s = DS18B20Manager::getInstance().getSensorByRole(SensorRole::boiler);

// Управление нагревателем
IHeater* heater = &ElectricHeater::getInstance();
heater->setPower(50);
heater->enableHeating(true);

// Переходы состояний
StateMachine::getInstance().start();
StateMachine::getInstance().emergencyStop();

// ΔT между датчиками
float delta_t = PhaseSelector::getInstance().getDeltaT();
```

---

## Зависимости

```
bblanchon/ArduinoJson @ ^7.0.4
paulstoffregen/OneWire @ ^2.3.8
milesburton/DallasTemperature @ ^3.11.0
knolleary/PubSubClient @ ^2.8
esphome/AsyncTCP-esphome @ ^2.0.0
esphome/ESPAsyncWebServer-esphome @ ^3.2.2
```