#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <vector>

// Режимы поведения AP
enum class ApBehavior : uint8_t {
    BOOT_NO_CONN = 0,    // AP при загрузке если нет подключения
    NO_CONN = 1,         // AP при любом отключении
    ALWAYS = 2,          // AP всегда включён
    BUTTON_ONLY = 3,     // AP только по команде
    TEMPORARY = 4        // Временный AP (отключается через таймаут)
};

struct WiFiNetwork {
    String ssid;
    int8_t rssi;
    uint8_t channel;
    wifi_auth_mode_t encryption;
    uint8_t* bssid;
};

struct WiFiConfig {
    char ssid[33];
    char password[65];
    IPAddress staticIP;
    IPAddress gateway;
    IPAddress subnet;
    bool useStaticIP;

    WiFiConfig() : staticIP(INADDR_NONE), gateway(INADDR_NONE), subnet(INADDR_NONE), useStaticIP(false) {
        memset(ssid, 0, sizeof(ssid));
        memset(password, 0, sizeof(password));
    }
};

class WiFiPortal {
public:
    WiFiPortal();

    // Инициализация
    bool begin();
    void loop();

    // Настройки
    void setApBehavior(ApBehavior behavior) { _apBehavior = behavior; }
    ApBehavior getApBehavior() const { return _apBehavior; }
    
    void setApCredentials(const char* ssid, const char* password);
    void setApIP(const IPAddress& ip, const IPAddress& gateway, const IPAddress& subnet);
    void setApChannel(uint8_t channel) { _apChannel = channel; }
    void setApHidden(bool hidden) { _apHidden = hidden; }
    
    void setStaCredentials(const char* ssid, const char* password);
    void setStaStaticIP(const IPAddress& ip, const IPAddress& gateway, const IPAddress& subnet);
    
    void setTxPower(wifi_power_t power) { _txPower = power; }
    void setHostname(const char* hostname) { _hostname = hostname; }
    void setConnectTimeout(uint32_t ms) { _connectTimeout = ms; }
    void setApTimeout(uint32_t ms) { _apTimeout = ms; } // для TEMPORARY режима

    // Управление
    bool connectSTA();
    bool disconnect();
    void startAP();
    void stopAP();
    void triggerAP() { startAP(); } // для BUTTON_ONLY режима
    void forceReconnect() { _forceReconnect = true; }

    // Сканирование
    bool startScan();
    std::vector<WiFiNetwork> getScanResults() const;
    bool isScanning() const { return WiFi.scanComplete() < 0 && WiFi.scanComplete() != WIFI_SCAN_FAILED; }

    // Состояние
    bool isConnected() const;
    bool isApActive() const { return _apActive; }
    String getApSSID() const;
    String getLocalIP() const;
    String getStaSSID() const;
    int8_t getSignalQuality() const;
    uint8_t getApClientCount() const;

    // События
    using EventCallback = void(*)(WiFiPortal* portal, const char* event);
    void onEvent(EventCallback callback) { _eventCallback = callback; }

    // Captive Portal DNS
    DNSServer& getDNSServer() { return _dnsServer; }

    // Временной AP
    void setTemporaryAPTimeout(uint32_t ms) { _apTimeout = ms; }

private:
    // Состояние
    ApBehavior _apBehavior = ApBehavior::BOOT_NO_CONN;
    bool _apActive = false;
    bool _forceReconnect = false;
    bool _wasConnected = false;
    unsigned _selectedNetwork = 0;
    uint32_t _lastConnectAttempt = 0;
    uint32_t _lastReconnectAttempt = 0;
    uint32_t _apStartTime = 0;
    bool _staConnected = false;

    // Настройки AP
    char _apSSID[33];
    char _apPassword[65];
    IPAddress _apIP;
    IPAddress _apGateway;
    IPAddress _apSubnet;
    uint8_t _apChannel = 1;
    bool _apHidden = false;
    uint32_t _apTimeout = 300000; // 5 минут

    // Настройки STA
    std::vector<WiFiConfig> _networks;
    wifi_power_t _txPower = WIFI_POWER_19_5dBm;
    const char* _hostname = nullptr;
    uint32_t _connectTimeout = 15000;

    // DNS сервер
    DNSServer _dnsServer;

    // Callback
    EventCallback _eventCallback = nullptr;

    // Внутренние методы
    void initWiFi();
    bool attemptConnection();
    void handleAPBehavior();
    void handleSTAReconnect();
    void handleTemporaryAP();
    void onWiFiEvent(WiFiEvent_t event);
    void emitEvent(const char* event);
    
    // Captive Portal
    void startCaptivePortal();
    void stopCaptivePortal();
    bool isCaptiveRequest(const String& host);

    friend void WiFiPortalEventHandler(WiFiEvent_t event, WiFiEventInfo_t info);
};

// Глобальный экземпляр
extern WiFiPortal wifiPortal;

#endif // WIFI_PORTAL_H
