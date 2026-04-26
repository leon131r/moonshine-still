# Задача для Ассистента 2: Веб-интерфейс (SPA)

> Создано: 2025-04-11

---

## Задача

Создать полноценный SPA (Single Page Application) для системы управления ректификационной колонной.

**Файлы:**
- `src/ui/web_assets/index.html`
- `src/ui/web_assets/css/style.css`
- `src/ui/web_assets/js/app.js`

---

## Технические требования

### 1. Без внешних зависимостей
- **НЕ использовать** CDN (Bootstrap, jQuery и т.д.)
- Всё локально: CSS и JS в отдельных файлах
- Автономная работа (ESP32 раздаёт WiFi, нет интернета)

### 2. WebSocket для real-time обновлений
- Подключение к `ws://<ip>/ws`
- Формат телеметрии (сервер → клиент):
```json
{
  "type": "telemetry",
  "phase": "body",
  "delta_t": 0.85,
  "heater_power": 45,
  "pid_out_cube": 45.2,
  "pid_out_cooler": 30.1,
  "heater_type": "electric",
  "heater_enabled": true,
  "errors": 0,
  "calibrating": false,
  "uptime": 3600,
  "ts": 1234567890,
  "temps": [
    {"role": "column_top", "name": "Верх колонны", "temp": 78.20, "raw": 78.15, "present": true, "offset": 0.05},
    {"role": "head_selection", "name": "Узел отбора голов", "temp": 78.50, "raw": 78.40, "present": true, "offset": 0.10},
    {"role": "body_selection", "name": "Узел отбора тела", "temp": 78.90, "raw": 78.85, "present": true, "offset": 0.05},
    {"role": "cooler", "name": "Охлаждающая жидкость", "temp": 25.00, "raw": 25.00, "present": true, "offset": 0.0}
  ]
}
```

### 3. Команды (клиент → сервер)
```json
{"cmd": "set_power", "value": 50}
{"cmd": "emergency_stop"}
{"cmd": "reset_error"}
{"cmd": "calibrate"}
{"cmd": "start"}
{"cmd": "set_phase", "phase": "body"}
{"cmd": "set_threshold", "heads_end": 1.2, "body_end": 0.3, "hysteresis": 0.1}
{"cmd": "set_pid", "target": "cube", "kp": 2.0, "ki": 0.5, "kd": 1.0}
```

### 4. Ответы сервера
```json
{"success": true, "message": "Power set"}
{"success": false, "message": "Unknown command"}
```

### 5. HTTP API (для первоначальной загрузки)
- `GET /api/status/snapshot` — начальное состояние
- `GET /api/config` — настройки
- `GET /api/log/latest?n=50` — логи
- `POST /api/command` — команды

---

## Вкладки интерфейса

### 1. Dashboard (Главная)
- **Температуры**: 4 датчика с графиками в реальном времени (Canvas)
  - T_column_top (верх колонны)
  - T_head_selection (узел отбора голов)
  - T_body_selection (узел отбора тела)
  - T_cooler (охлаждающая жидкость)
- **Фаза**: визуальный индикатор (цветной бейдж)
  - IDLE → серый
  - HEATING → оранжевый
  - HEADS → жёлтый
  - BODY → зелёный
  - TAILS → коричневый
  - FINISH → синий
  - ERROR → красный мигающий
- **ΔT**: большое число (разница head_selection - body_selection)
- **Мощность нагрева**: progress bar (0-100%)
- **PID выходы**: два индикатора (куб, охладитель)
- **Uptime**: таймер
- **Статус подключения**: индикатор WebSocket (зелёный/красный)

### 2. Управление
- **Кнопка «Запуск»** — начать процесс
- **Кнопка «Аварийная остановка»** — большая красная, с подтверждением
- **Кнопка «Сброс аварии»** — после emergency_stop
- **Кнопка «Калибровка»** — запуск калибровки датчиков
- **Ручная мощность**: ползунок 0-100% + кнопка «Применить»
- **Ручная фаза**: выпадающий список + кнопка «Установить»
- **Статус калибровки**: прогресс-бар (если идёт)

### 3. Настройки
- **Пороги фаз**:
  - heads_end (число, °C)
  - body_end (число, °C)
  - delta_hysteresis (число, °C)
  - Кнопка «Сохранить»
- **PID куба**:
  - Kp, Ki, Kd (числа)
  - out_min, out_max, integral_max
  - Кнопка «Сохранить»
- **PID охладителя**:
  - Kp, Ki, Kd (числа)
  - out_min, out_max, integral_max
  - Кнопка «Сохранить»
- **MQTT**:
  - enabled (чекбокс)
  - host, port, user, pass
  - topic_root
- **WiFi**:
  - SSID
- **TFT**:
  - enabled (чекбокс)
- Кнопка «Сохранить всё» → POST /api/command с несколькими командами

### 4. Логи
- Таблица последних записей (загрузка через GET /api/log/latest?n=100)
- Колонки: время, фаза, T_top, T_select, T_below, pid_out, heater_type, errors
- Фильтр по фазе (выпадающий список)
- Кнопка «Скачать NDJSON» — сохранение файла
- Автообновление каждые 10 секунд

### 5. AI (опционально)
- Поле ввода: команда для ИИ-агента
- Кнопка «Отправить»
- История команд (последние 20)
- Статус ИИ-агента

---

## Дизайн

### Цветовая схема
- Фон: #1a1a2e (тёмно-синий)
- Карточки: #16213e
- Акцент: #0f3460
- Текст: #e0e0e0
- Зелёный (OK): #00c853
- Красный (Error): #ff1744
- Оранжевый (Warning): #ff9100
- Жёлтый (Heads): #ffd600

### Адаптивность
- Мобильные устройства (320px+) — одна колонка
- Планшет (768px+) — две колонки
- Десктоп (1024px+) — три колонки

### Без фреймворков
- Чистый CSS (Grid/Flexbox)
- Без CSS-препроцессоров
- Vanilla JavaScript (ES6+)
- Без TypeScript

### Графики
- Canvas API для отрисовки графиков температур
- Хранить последние 120 точек (1 минута при обновлении каждые 500мс)
- Цветные линии для каждого датчика
- Ось Y — температура, ось X — время

---

## JavaScript архитектура

```javascript
// app.js
const App = {
  ws: null,
  telemetryHistory: [],  // Последние 120 точек
  maxHistory: 120,
  
  init() {
    this.connectWS();
    this.loadInitialData();
    this.bindEvents();
  },
  
  connectWS() {
    // ws://<current_host>/ws
    // onmessage → updateTelemetry(data)
    // onclose → reconnect через 3 сек
  },
  
  sendCommand(cmd, params) {
    // JSON.stringify и ws.send()
  },
  
  updateTelemetry(data) {
    // Обновить UI: температуры, фаза, мощность, ΔT
    // Добавить в историю для графиков
  },
  
  loadInitialData() {
    // fetch('/api/status/snapshot')
    // fetch('/api/config')
  },
  
  bindEvents() {
    // Кнопки, формы, ползунки
  },
  
  drawCharts() {
    // Canvas отрисовка
  }
};

document.addEventListener('DOMContentLoaded', () => App.init());
```

---

## Зависимости

| Модуль | Что предоставляет |
|--------|-------------------|
| `web_api.cpp` | HTTP endpoints + WebSocket сервер |
| `state_machine.h` | Фаза процесса (в telemetry) |
| `ds18b20_manager.h` | Температуры (в telemetry) |
| `settings.h` | Настройки (через /api/config) |

---

## Примечания

- Все файлы в `src/ui/web_assets/` будут загружены в LittleFS
- `index.html` ссылается на `css/style.css` и `js/app.js`
- При загрузке `GET /` → сервер отдаёт `index.html`
- Пути в HTML относительные: `./css/style.css`, `./js/app.js`
- Не использовать `fetch` с абсолютными URL — только относительные

---

## Ожидаемый результат

1. **index.html** — структура с 5 вкладками
2. **css/style.css** — стили, адаптивность, цветовая схема
3. **js/app.js** — логика: WebSocket, графики, команды, UI

Готовый интерфейс должен позволять:
- Видеть телеметрию в реальном времени
- Управлять процессом (старт, стоп, калибровка)
- Настраивать пороги и PID
- Смотреть логи
- Аварийная остановка в один клик
