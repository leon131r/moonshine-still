# Восстановление работоспособности датчиков

## Что было сломано

После изменений в `ds18b20_manager.cpp` и `web_api.cpp` перестали работать:
- Сканирование датчиков
- Отображение в таблице
- Сохранение настроек

## Что было исправлено

### 1. Откат изменений

```bash
git checkout -- src/sensors/ds18b20_manager.cpp
git checkout -- src/main.cpp
git checkout -- src/comm/web_api.cpp
git checkout -- src/comm/web_api.h
```

### 2. Добавление команды scan_sensors в web_api.cpp

В двух местах (HTTP и WebSocket):

```cpp
// После set_sensor_active
if (strcmp(cmd, "scan_sensors") == 0) {
    JsonDocument doc_response;
    doc_response["type"] = "sensors";
    JsonArray arr = doc_response["sensors"].to<JsonArray>();
    
    const SensorData* s_array = sensors_->getSensors();
    uint8_t count = sensors_->getSensorCount();
    
    for (uint8_t i = 0; i < count && i < 4; i++) {
        JsonObject s = arr.add<JsonObject>();
        s["address"] = s_array[i].address_hex;
        s["present"] = s_array[i].present;
        s["temp"] = s_array[i].temp_corrected;
        s["role"] = roleToString(s_array[i].role);
        s["active"] = s_array[i].active;
        s["name"] = s_array[i].name;
    }
    
    String output;
    serializeJson(doc_response, output);
    request->send(200, "application/json", output);
    // или client->text(output) для WebSocket
    return;
}
```

### 3. Добавление обработки в app.js

```javascript
// В onmessage:
if (d.type === 'sensors' && d.sensors) {
    this.updateSensorsTable(d.sensors);
}

// Новая функция:
updateSensorsTable(sensors) {
    const tbody = document.querySelector('#sensorsTable tbody');
    if (!tbody) return;
    
    tbody.innerHTML = '';
    sensors.forEach((s, idx) => {
        const tr = document.createElement('tr');
        tr.dataset.idx = idx;
        const tempVal = s.temp && s.temp > 0 ? s.temp.toFixed(2) : '--';
        const offsetVal = s.offset || s.manual_offset || 0;
        tr.innerHTML = `<td style="font-family:monospace;font-size:11px">${s.address || '—'}</td>
            <td><select class="role-select" data-idx="${idx}">
                <option value="column_top" ${s.role === 'column_top' ? 'selected' : ''}>column_top</option>
                <option value="head_selection" ${s.role === 'head_selection' ? 'selected' : ''}>head_selection</option>
                <option value="body_selection" ${s.role === 'body_selection' ? 'selected' : ''}>body_selection</option>
                <option value="coiler" ${s.role === 'coiler' ? 'selected' : ''}>coiler</option>
                <option value="boiler" ${s.role === 'boiler' ? 'selected' : ''}>boiler</option>
            </select></td>
            <td><input type="text" class="name-input" data-idx="${idx}" value="${s.name || ''}" style="width:80px;font-size:11px"></td>
            <td>${tempVal}°C</td>
            <td><input type="number" class="offset-input" data-idx="${idx}" value="${offsetVal}" step="0.01" style="width:50px;font-size:11px"></td>
            <td><input type="checkbox" class="calib-check" data-idx="${idx}" ${s.calibrate ? 'checked' : ''}></td>
            <td><input type="checkbox" class="active-check" data-idx="${idx}" ${s.active !== false ? 'checked' : ''}></td>
            <td><button class="btn btn-xs save-sensor" data-idx="${idx}">💾</button></td>`;
        tbody.appendChild(tr);
    });
    this.toast(`Найдено датчиков: ${sensors.length}`, 'ok');
}
```

### 4. Минимизация app.js

```bash
npx terser data/app.js -o data/app.min.js
```

### 5. Загрузка

```bash
pio run --target upload --upload-port /dev/ttyACM0
pio run --target uploadfs --upload-port /dev/ttyACM0
```

**ВНИМАНИЕ:** uploadfs стирает настройки! Нужно через веб-интерфейс ввести WiFi заново.

## Проверка

1. Подключиться к ESP32 через браузер
2. Нажать "Сканировать"
3. Таблица должна показать датчики с адресами
4. Можно редактировать роли и названия
5. Кнопка 💾 сохраняет изменения