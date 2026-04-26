#include "WiFiPortal.h"

WiFiPortal wifiPortal;

// Глобальный обработчик событий WiFi
void WiFiPortalEventHandler(WiFiEvent_t event, WiFiEventInfo_t info) {
    wifiPortal.onWiFiEvent(event);
}

WiFiPortal::WiFiPortal()
    : _apIP(4, 3, 2, 1), _apGateway(4, 3, 2, 1), _apSubnet(255, 255, 255, 0) {
    // Генерируем уникальный AP SSID из MAC
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_apSSID, sizeof(_apSSID), "ESP-AP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    strcpy(_apPassword, "config123");
}

bool WiFiPortal::begin() {
    Serial.println("[WiFiPortal] Initializing...");

    // Критический порядок инициализации для ESP32
    WiFi.persistent(false);
    // НЕ включаем autoReconnect — будем управлять вручную чтобы не блокировать loop
    WiFi.onEvent(WiFiPortalEventHandler);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#endif

    // ВАЖНО: hostname ДО mode() — критично для ESP32-C3/S3
    if (_hostname) {
        WiFi.setHostname(_hostname);
        Serial.printf("[WiFiPortal] Hostname: %s\n", _hostname);
    }

    // Переключаемся в STA режим
    WiFi.mode(WIFI_STA);
    delay(50);

    // Пробуем подключиться к STA
    if (_networks.size() > 0 && strlen(_networks[0].ssid) > 0) {
        Serial.printf("[WiFiPortal] Attempting STA connection to: %s\n", _networks[0].ssid);
        if (connectSTA()) {
            // Успешно подключились - AP не нужен (если не ALWAYS)
            if (_apBehavior == ApBehavior::ALWAYS) {
                startAP();
            }
            return true;
        }
        Serial.println("[WiFiPortal] STA connection failed");
    } else {
        Serial.println("[WiFiPortal] No STA credentials configured");
    }

    // Не удалось подключиться или нет настроек — запускаем AP
    if (_apBehavior != ApBehavior::BUTTON_ONLY) {
        Serial.println("[WiFiPortal] Starting AP (STA failed/no credentials)");
        startAP();
    } else {
        Serial.println("[WiFiPortal] AP not started (BUTTON_ONLY mode)");
    }

    return true;
}

void WiFiPortal::loop() {
    // Обработка DNS для Captive Portal
    if (_apActive) {
        _dnsServer.processNextRequest();
    }

    // Обработка поведения AP
    handleAPBehavior();

    // Переподключение STA — только если credentials заданы
    if (!_staConnected && _networks.size() > 0 && strlen(_networks[0].ssid) > 0) {
        handleSTAReconnect();
    }

    // Временной AP
    if (_apBehavior == ApBehavior::TEMPORARY && _apActive) {
        handleTemporaryAP();
    }
}

bool WiFiPortal::connectSTA() {
    if (_networks.empty()) return false;

    WiFiConfig& net = _networks[_selectedNetwork];

    Serial.printf("[WiFiPortal] Connecting to %s...\n", net.ssid);

    if (net.useStaticIP && net.staticIP != INADDR_NONE) {
        WiFi.config(net.staticIP, net.gateway, net.subnet);
    }

    // ВАЖНО: setTxPower МЕЖДУ mode() и begin()
    WiFi.setTxPower(_txPower);
    WiFi.begin(net.ssid, net.password);

    uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < _connectTimeout) {
        delay(100);
        yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
        _staConnected = true;
        _wasConnected = true;
        Serial.printf("[WiFiPortal] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        emitEvent("connected");
        return true;
    }

    Serial.println("[WiFiPortal] Connection timeout");
    _staConnected = false;
    emitEvent("connect_failed");
    return false;
}

void WiFiPortal::handleAPBehavior() {
    switch (_apBehavior) {
        case ApBehavior::BOOT_NO_CONN:
            // AP только при загрузке если нет подключения
            if (_staConnected && _apActive) {
                stopAP();
            }
            break;

        case ApBehavior::NO_CONN:
            // AP при любом отключении
            if (!_staConnected && !_apActive) {
                startAP();
            } else if (_staConnected && _apActive) {
                stopAP();
            }
            break;

        case ApBehavior::ALWAYS:
            // AP всегда
            if (!_apActive) {
                startAP();
            }
            break;

        case ApBehavior::BUTTON_ONLY:
            // Ничего не делаем автоматически
            break;

        case ApBehavior::TEMPORARY:
            // Обработано в handleTemporaryAP()
            break;
    }
}

void WiFiPortal::handleSTAReconnect() {
    uint32_t now = millis();

    // Переподключение каждые 60 секунд
    if (now - _lastReconnectAttempt < 60000 && !_forceReconnect) {
        return;
    }
    
    // ВАЖНО: НЕ реконнектим если STA credentials не заданы
    if (_networks.empty() || strlen(_networks[0].ssid) == 0) {
        return; // Нечего подключать — STA не настроен
    }
    
    // Если уже подключены к STA — НЕ реконнектим!
    if (_staConnected && WiFi.status() == WL_CONNECTED) {
        return;
    }
    
    _lastReconnectAttempt = now;
    _forceReconnect = false;

    WiFiConfig& net = _networks[_selectedNetwork];
    Serial.printf("[WiFiPortal] Attempting STA reconnect to: %s\n", net.ssid);
    
    // ВАЖНО: НЕ вызываем WiFi.disconnect()! Это убивает радио на ESP32-S3.
    // Просто вызываем WiFi.begin() — он сам переподключится
    
    if (net.useStaticIP && net.staticIP != INADDR_NONE) {
        WiFi.config(net.staticIP, net.gateway, net.subnet);
    }
    
    WiFi.begin(net.ssid, net.password);
    // Результат придёт через WiFiEvent — STA_CONNECTED или STA_DISCONNECTED
}

void WiFiPortal::handleTemporaryAP() {
    if (!_apActive) return;

    uint32_t apDuration = millis() - _apStartTime;
    
    // Отключаем AP после таймаута если нет клиентов
    if (apDuration > _apTimeout && getApClientCount() == 0) {
        // Только в первые 10 минут
        if (apDuration < _apTimeout * 2) {
            Serial.println("[WiFiPortal] Temporary AP timeout, stopping...");
            stopAP();
        }
    }

    // Если подключились к STA - отключаем AP
    if (_staConnected) {
        stopAP();
    }
}

void WiFiPortal::startAP() {
    if (_apActive) return;

    Serial.printf("[WiFiPortal] Starting AP: %s (channel %d)...\n", _apSSID, _apChannel);

    // КРИТИЧЕСКИЙ ПОРЯДОК для ESP32-S3/C3:
    // 1. WiFi.mode(WIFI_AP_STA) — переключаем режим
    // 2. delay(100) — даем время на переключение
    // 3. WiFi.setTxPower() — мощность ДО softAP()
    // 4. WiFi.softAPConfig() — конфигурация IP
    // 5. WiFi.softAP() — запуск AP

    // Шаг 1: Переключаемся в AP_STA режим
    WiFi.mode(WIFI_AP_STA);
    delay(100); // Увеличенная задержка для стабильности

    // Шаг 2: Устанавливаем мощность ПЕРЕД softAP()
    // ВАЖНО: для ESP32-C3/S3 не ставить максимальную мощность!
    wifi_power_t safePower = _txPower;
    if (_txPower > WIFI_POWER_8_5dBm) {
        safePower = WIFI_POWER_8_5dBm;
        Serial.println("[WiFiPortal] Reducing TX power to 8.5dBm for AP stability");
    }
    WiFi.setTxPower(safePower);
    delay(10);

    // Шаг 3: Конфигурация IP
    WiFi.softAPConfig(_apIP, _apGateway, _apSubnet);
    delay(10);

    // Шаг 4: Запускаем AP
    // Параметры: SSID, password, channel, ssid_hidden, max_connection
    bool apResult = WiFi.softAP(_apSSID, _apPassword, _apChannel, _apHidden, 4);
    
    if (!apResult) {
        Serial.println("[WiFiPortal] ERROR: Failed to start AP!");
        _apActive = false;
        emitEvent("ap_failed");
        return;
    }

    _apActive = true;
    _apStartTime = millis();

    Serial.printf("[WiFiPortal] AP started: IP=%s, SSID=%s\n", 
                  WiFi.softAPIP().toString().c_str(), _apSSID);

    // Запускаем Captive Portal DNS
    startCaptivePortal();

    emitEvent("ap_started");
}

void WiFiPortal::stopAP() {
    if (!_apActive) return;

    Serial.println("[WiFiPortal] Stopping AP...");
    stopCaptivePortal();
    WiFi.softAPdisconnect(true);
    _apActive = false;
    emitEvent("ap_stopped");
}

void WiFiPortal::startCaptivePortal() {
    _dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    _dnsServer.start(53, "*", _apIP);
    Serial.println("[WiFiPortal] Captive Portal started");
}

void WiFiPortal::stopCaptivePortal() {
    _dnsServer.stop();
    Serial.println("[WiFiPortal] Captive Portal stopped");
}

bool WiFiPortal::isCaptiveRequest(const String& host) {
    // Проверяем является ли хост IP адресом
    bool isIP = true;
    for (size_t i = 0; i < host.length(); i++) {
        char c = host.charAt(i);
        if (c != '.' && (c < '0' || c > '9')) {
            isIP = false;
            break;
        }
    }

    // Если не IP и не содержит известные хосты - это captive запрос
    return !isIP && 
           host.indexOf("wled.me") < 0 && 
           host.indexOf(':') < 0;
}

bool WiFiPortal::startScan() {
    if (isScanning()) return false;

    WiFi.scanDelete();
    WiFi.scanNetworks(true); // Асинхронное сканирование
    return true;
}

std::vector<WiFiNetwork> WiFiPortal::getScanResults() const {
    std::vector<WiFiNetwork> results;
    int16_t count = WiFi.scanComplete();

    if (count <= 0) return results;

    results.reserve(count);
    for (int i = 0; i < count; i++) {
        WiFiNetwork net;
        net.ssid = WiFi.SSID(i);
        net.rssi = WiFi.RSSI(i);
        net.channel = WiFi.channel(i);
        net.encryption = WiFi.encryptionType(i);
        results.push_back(net);
    }

    // Перезапускаем сканирование для следующего раза
    WiFi.scanDelete();
    WiFi.scanNetworks(true);

    return results;
}

void WiFiPortal::onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("[WiFiPortal] STA Connected");
            _staConnected = true;
            emitEvent("sta_connected");
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("[WiFiPortal] STA Disconnected");
            _staConnected = false;
            emitEvent("sta_disconnected");
            break;

        case ARDUINO_EVENT_WIFI_AP_START:
            Serial.println("[WiFiPortal] AP Started");
            emitEvent("ap_started");
            break;

        case ARDUINO_EVENT_WIFI_AP_STOP:
            Serial.println("[WiFiPortal] AP Stopped");
            emitEvent("ap_stopped");
            break;

        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("[WiFiPortal] Client connected to AP");
            emitEvent("ap_client_connected");
            break;

        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("[WiFiPortal] Client disconnected from AP");
            emitEvent("ap_client_disconnected");
            break;

        default:
            break;
    }
}

// Обработчик кастомных событий (вызывается из startAP/stopAP)

void WiFiPortal::emitEvent(const char* event) {
    if (_eventCallback) {
        _eventCallback(this, event);
    }
}

// Публичные методы
bool WiFiPortal::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiPortal::getApSSID() const {
    return String(_apSSID);
}

String WiFiPortal::getLocalIP() const {
    if (_apActive) {
        return WiFi.softAPIP().toString();
    }
    return WiFi.localIP().toString();
}

String WiFiPortal::getStaSSID() const {
    if (_networks.empty()) return "";
    return String(_networks[_selectedNetwork].ssid);
}

int8_t WiFiPortal::getSignalQuality() const {
    int rssi = WiFi.RSSI();
    if (rssi <= -100) return 0;
    if (rssi >= -50) return 100;
    return 2 * (rssi + 100);
}

uint8_t WiFiPortal::getApClientCount() const {
#ifdef ARDUINO_ARCH_ESP32
    return WiFi.softAPgetStationNum();
#else
    return wifi_softap_get_station_num();
#endif
}

void WiFiPortal::setApCredentials(const char* ssid, const char* password) {
    strncpy(_apSSID, ssid, sizeof(_apSSID) - 1);
    strncpy(_apPassword, password, sizeof(_apPassword) - 1);
}

void WiFiPortal::setApIP(const IPAddress& ip, const IPAddress& gateway, const IPAddress& subnet) {
    _apIP = ip;
    _apGateway = gateway;
    _apSubnet = subnet;
}

void WiFiPortal::setStaCredentials(const char* ssid, const char* password) {
    if (_networks.empty()) {
        _networks.push_back(WiFiConfig());
    }
    strncpy(_networks[0].ssid, ssid, sizeof(_networks[0].ssid) - 1);
    strncpy(_networks[0].password, password, sizeof(_networks[0].password) - 1);
}

void WiFiPortal::setStaStaticIP(const IPAddress& ip, const IPAddress& gateway, const IPAddress& subnet) {
    if (_networks.empty()) {
        _networks.push_back(WiFiConfig());
    }
    _networks[0].staticIP = ip;
    _networks[0].gateway = gateway;
    _networks[0].subnet = subnet;
    _networks[0].useStaticIP = true;
}

bool WiFiPortal::disconnect() {
    WiFi.disconnect();
    _staConnected = false;
    return true;
}
