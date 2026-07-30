#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <RTClib.h>

// =====================================================
// CASON SOLAR SAFETY CONTROLLER
// ESP32 -> RENDER -> LINE
// CH1 power, CH2 green, CH3 yellow, CH4 red, CH5 sound
// DI1 safety NC, DI2 warning NC
// =====================================================

// -----------------------------------------------------
// Wi-Fi
// -----------------------------------------------------
const char *WIFI_SETUP_AP_NAME = "CASON-SETUP";
const char *WIFI_SETUP_AP_PASSWORD = "cason1234";

// -----------------------------------------------------
// LINE Command Server / Webhook Bridge
// LINE -> Webhook Server -> ESP32 polls commands
// -----------------------------------------------------
const char *COMMAND_SERVER_BASE_URL = "https://casonpower.onrender.com";
const char *DEVICE_ID = "CASON-0001";
constexpr bool COMMAND_SERVER_ENABLED = true;
constexpr uint32_t COMMAND_POLL_INTERVAL_MS = 3000;
constexpr uint32_t SERVER_HEARTBEAT_INTERVAL_MS = 30000;

// -----------------------------------------------------
// Waveshare TCA9554 Relay Controller
// -----------------------------------------------------
constexpr uint8_t I2C_SDA_PIN = 42;
constexpr uint8_t I2C_SCL_PIN = 41;
constexpr uint8_t TCA9554_ADDRESS = 0x20;

constexpr uint8_t TCA_OUTPUT_REG = 0x01;
constexpr uint8_t TCA_POLARITY_REG = 0x02;
constexpr uint8_t TCA_CONFIG_REG = 0x03;

constexpr bool RELAY_ACTIVE_HIGH = true;

constexpr uint8_t RELAY_CH1_POWER = 1;
constexpr uint8_t RELAY_CH2_GREEN = 2;
constexpr uint8_t RELAY_CH3_YELLOW = 3;
constexpr uint8_t RELAY_CH4_RED = 4;
constexpr uint8_t RELAY_CH5_SOUND = 5;

// -----------------------------------------------------
// Digital Inputs
// DI1: safety alarm NC, normal LOW, active HIGH
// DI2: warning dry contact NC, normal LOW, active HIGH
// -----------------------------------------------------
constexpr uint8_t DI1_PIN = 4;
constexpr uint8_t DI1_ACTIVE_LEVEL = HIGH;
constexpr uint8_t DI1_INPUT_MODE = INPUT_PULLUP;

constexpr uint8_t DI2_PIN = 5;
constexpr uint8_t DI2_ACTIVE_LEVEL = HIGH;
constexpr uint8_t DI2_INPUT_MODE = INPUT_PULLUP;

constexpr uint32_t DI_DEBOUNCE_MS = 150;
constexpr uint32_t RELAY_RESTORE_DELAY_MS = 30000;
constexpr uint32_t HEARTBEAT_MS = 1000;
constexpr uint32_t SERVER_RETRY_DELAY_MS = 30000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t HTTP_TIMEOUT_MS = 3000;

// -----------------------------------------------------
// ตัวแปรระบบ
// -----------------------------------------------------
uint8_t relayOutput = 0x00;

bool relay1On = false;
bool alarmActive = false;

bool di1Active = false;
bool di1LastActiveState = false;
bool di2Active = false;
bool di2LastActiveState = false;
bool relayRestorePending = false;

enum IndicatorState
{
    INDICATOR_UNKNOWN,
    INDICATOR_NORMAL,
    INDICATOR_MINOR_FAULT,
    INDICATOR_MAJOR_FAULT
};

IndicatorState currentIndicatorState = INDICATOR_UNKNOWN;

uint32_t di1LastChangeTime = 0;
uint32_t di2LastChangeTime = 0;
uint32_t relayRestoreStartTime = 0;
uint32_t lastHeartbeatTime = 0;
uint32_t lastCommandPollTime = 0;
uint32_t lastServerHeartbeatTime = 0;
uint32_t lastWiFiPortalTime = 0;
uint32_t lastWiFiConnectAttemptTime = 0;
bool wifiSetupPortalRunning = false;
bool wifiResetPending = false;
uint32_t wifiResetRequestTime = 0;

WiFiManager wifiManager;
RTC_DS3231 rtc;
bool rtcAvailable = false;
bool rtcSetFromBuildTime = false;

// คิวเหตุการณ์ที่จะส่งผ่าน Render ไป LINE
String pendingEvent;
String pendingStatus;
String pendingMessage;

bool serverMessagePending = false;
uint32_t nextServerAttemptTime = 0;

int readDI1Raw();
bool readDI1();
int readDI2Raw();
bool readDI2();
const char *di1StateText(bool active);
void printDI1Detail(const char *prefix);
void printDI2Detail(const char *prefix);
void updateStatusIndicators(const char *reason);
void processCommand(String command);
void showStatus();
void queueServerMessage(const String &eventName, const String &statusName, const String &message);
void updateServerQueue();
void resetWiFiSettings();
void updateWiFiPortal();
void updateServerHeartbeat();
String buildSystemCheckMessage();
bool checkCommandServer();
bool isRelayControllerOnline();
bool httpBeginForURL(HTTPClient &http, WiFiClient &plainClient, WiFiClientSecure &secureClient, const String &url);
bool isImportantLineEvent(const String &eventName);
void rtcBegin();
String rtcTimestamp();
String rtcStatusText();

// =====================================================
// TCA9554
// =====================================================

bool tcaWriteRegister(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(TCA9554_ADDRESS);
    Wire.write(reg);
    Wire.write(value);

    const uint8_t result = Wire.endTransmission();

    if (result != 0)
    {
        Serial.print("[I2C] Write failed, code=");
        Serial.println(result);
        return false;
    }

    return true;
}

bool tcaReadRegister(uint8_t reg, uint8_t &value)
{
    Wire.beginTransmission(TCA9554_ADDRESS);
    Wire.write(reg);

    const uint8_t result = Wire.endTransmission(false);

    if (result != 0)
    {
        Serial.print("[I2C] Read address failed, code=");
        Serial.println(result);
        return false;
    }

    if (Wire.requestFrom(TCA9554_ADDRESS, static_cast<uint8_t>(1)) != 1)
    {
        Serial.println("[I2C] Read failed: no data");
        return false;
    }

    value = Wire.read();
    return true;
}

bool isRelayControllerOnline()
{
    uint8_t value = 0;
    return tcaReadRegister(TCA_OUTPUT_REG, value);
}

bool tcaBegin()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);

    Wire.beginTransmission(TCA9554_ADDRESS);
    const uint8_t result = Wire.endTransmission();

    if (result != 0)
    {
        Serial.println("[TCA9554] NOT FOUND at 0x20");
        return false;
    }

    Serial.println("[TCA9554] FOUND at 0x20");

    if (!tcaWriteRegister(TCA_POLARITY_REG, 0x00))
    {
        return false;
    }

    // P0-P7 เป็น Output
    if (!tcaWriteRegister(TCA_CONFIG_REG, 0x00))
    {
        return false;
    }

    // เริ่มต้นให้รีเลย์ทุกช่อง OFF
    relayOutput = RELAY_ACTIVE_HIGH ? 0x00 : 0xFF;

    if (!tcaWriteRegister(TCA_OUTPUT_REG, relayOutput))
    {
        return false;
    }

    relay1On = false;

    Serial.println("[RELAY] All relays OFF");
    return true;
}

// =====================================================
// RTC DS3231
// =====================================================

void rtcBegin()
{
    rtcAvailable = rtc.begin();
    rtcSetFromBuildTime = false;

    if (!rtcAvailable)
    {
        Serial.println("[RTC] DS3231 not found");
        return;
    }

    Serial.println("[RTC] DS3231 found");

    if (rtc.lostPower())
    {
        Serial.println("[RTC] Lost power; setting time from firmware build");
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        rtcSetFromBuildTime = true;
    }

    Serial.print("[RTC] Time: ");
    Serial.println(rtcTimestamp());
}

String rtcTimestamp()
{
    if (!rtcAvailable)
    {
        return "RTC_NOT_AVAILABLE";
    }

    DateTime now = rtc.now();
    char buffer[24];
    snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d %02d:%02d:%02d",
        now.year(),
        now.month(),
        now.day(),
        now.hour(),
        now.minute(),
        now.second()
    );

    return String(buffer);
}

String rtcStatusText()
{
    if (!rtcAvailable)
    {
        return "NOT_FOUND";
    }

    if (rtcSetFromBuildTime)
    {
        return "SET_FROM_BUILD_TIME";
    }

    return "OK";
}

// =====================================================
// Relay
// =====================================================

bool setRelay(uint8_t channel, bool turnOn)
{
    if (channel < 1 || channel > 8)
    {
        Serial.println("[RELAY] Invalid channel");
        return false;
    }

    const uint8_t bitMask = 1U << (channel - 1);
    const bool outputHigh =
        RELAY_ACTIVE_HIGH ? turnOn : !turnOn;
    uint8_t nextRelayOutput = relayOutput;

    if (outputHigh)
    {
        nextRelayOutput |= bitMask;
    }
    else
    {
        nextRelayOutput &= static_cast<uint8_t>(~bitMask);
    }

    if (!tcaWriteRegister(TCA_OUTPUT_REG, nextRelayOutput))
    {
        return false;
    }

    relayOutput = nextRelayOutput;

    if (channel == 1)
    {
        relay1On = turnOn;
    }

    Serial.print("[RELAY] CH");
    Serial.print(channel);
    Serial.println(turnOn ? " ON" : " OFF");

    return true;
}

const char *indicatorStateText(IndicatorState state)
{
    switch (state)
    {
    case INDICATOR_NORMAL:
        return "NORMAL_GREEN";
    case INDICATOR_MINOR_FAULT:
        return "MINOR_YELLOW";
    case INDICATOR_MAJOR_FAULT:
        return "MAJOR_RED";
    default:
        return "UNKNOWN";
    }
}

bool setIndicatorRelays(
    bool greenOn,
    bool yellowOn,
    bool redOn,
    bool soundOn)
{
    bool ok = true;

    ok &= setRelay(RELAY_CH2_GREEN, greenOn);
    ok &= setRelay(RELAY_CH3_YELLOW, yellowOn);
    ok &= setRelay(RELAY_CH4_RED, redOn);
    ok &= setRelay(RELAY_CH5_SOUND, soundOn);

    return ok;
}

void updateStatusIndicators(const char *reason)
{
    IndicatorState nextState = INDICATOR_NORMAL;

    if (di1Active || readDI1())
    {
        nextState = INDICATOR_MAJOR_FAULT;
    }
    else if (di2Active || readDI2() ||
             alarmActive || relayRestorePending ||
             serverMessagePending ||
             WiFi.status() != WL_CONNECTED)
    {
        nextState = INDICATOR_MINOR_FAULT;
    }

    if (nextState == currentIndicatorState)
    {
        return;
    }

    currentIndicatorState = nextState;

    Serial.print("[STATUS-LIGHT] ");
    Serial.print(indicatorStateText(currentIndicatorState));
    Serial.print(" reason=");
    Serial.println(reason);

    setIndicatorRelays(
        currentIndicatorState == INDICATOR_NORMAL,
        currentIndicatorState == INDICATOR_MINOR_FAULT,
        currentIndicatorState == INDICATOR_MAJOR_FAULT,
        currentIndicatorState == INDICATOR_MAJOR_FAULT
    );
}

// =====================================================
// Wi-Fi
// =====================================================

bool connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (wifiSetupPortalRunning)
        {
            wifiManager.stopConfigPortal();
            wifiSetupPortalRunning = false;
            Serial.println("[WIFI] Setup portal stopped after connection");
        }
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.setSleep(false);

    if (wifiSetupPortalRunning)
    {
        return false;
    }

    const uint32_t now = millis();
    if (lastWiFiConnectAttemptTime != 0 && now - lastWiFiConnectAttemptTime < 10000)
    {
        return false;
    }

    lastWiFiConnectAttemptTime = now;

    Serial.println("[WIFI] Trying saved credentials");
    WiFi.begin();

    const uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 500)
    {
        delay(10);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("[WIFI] Connected, IP=");
        Serial.println(WiFi.localIP());
        return true;
    }

    if (lastWiFiPortalTime != 0 && now - lastWiFiPortalTime < 300000)
    {
        Serial.println("[WIFI] Setup portal cooldown; safety loop continues");
        return false;
    }

    lastWiFiPortalTime = now;

    Serial.print("[WIFI] Starting non-blocking setup portal AP=");
    Serial.println(WIFI_SETUP_AP_NAME);
    Serial.println("[WIFI] Connect phone to CASON-SETUP and open 192.168.4.1");

    wifiManager.setConfigPortalBlocking(false);
    wifiManager.setConfigPortalTimeout(180);
    wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
    wifiManager.setDebugOutput(false);

    const bool connected = wifiManager.autoConnect(
        WIFI_SETUP_AP_NAME,
        WIFI_SETUP_AP_PASSWORD
    );

    if (connected)
    {
        Serial.print("[WIFI] Connected, IP=");
        Serial.println(WiFi.localIP());
        return true;
    }

    wifiSetupPortalRunning = true;
    Serial.println("[WIFI] Setup portal running; safety loop continues");
    return false;
}

void updateWiFiPortal()
{
    if (!wifiSetupPortalRunning)
    {
        return;
    }

    wifiManager.process();

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiManager.stopConfigPortal();
        wifiSetupPortalRunning = false;
        Serial.print("[WIFI] Connected from setup portal, IP=");
        Serial.println(WiFi.localIP());
        return;
    }

    if (millis() - lastWiFiPortalTime >= 180000)
    {
        wifiManager.stopConfigPortal();
        wifiSetupPortalRunning = false;
        WiFi.mode(WIFI_STA);
        Serial.println("[WIFI] Setup portal timeout; safety loop continues");
    }
}

void resetWiFiSettings()
{
    if (wifiResetPending)
    {
        return;
    }

    Serial.println("[WIFI] Wi-Fi reset requested");

    wifiResetPending = true;
    wifiResetRequestTime = millis();

    queueServerMessage(
        "WIFI_RESET",
        "NORMAL",
        "รับคำสั่งล้างค่า Wi-Fi แล้ว\nระบบจะรีสตาร์ทและเปิด CASON-SETUP ถ้าต่อ Wi-Fi เดิมไม่ได้"
    );
}

void updateWiFiReset()
{
    if (!wifiResetPending)
    {
        return;
    }

    if (millis() - wifiResetRequestTime < 5000)
    {
        return;
    }

    Serial.println("[WIFI] Reset saved Wi-Fi settings now");

    wifiManager.resetSettings();
    WiFi.disconnect(true, true);
    delay(500);

    ESP.restart();
}

// =====================================================
// Render Event Bridge
// =====================================================

String buildServerEventPayload(
    const String &eventName,
    const String &statusName,
    const String &message)
{
    JsonDocument document;

    document["device"] = DEVICE_ID;
    document["controller"] = "Cason Solar Safety Controller";
    document["event"] = eventName;
    document["type"] = eventName;
    document["status"] = statusName;
    document["message"] = message;
    document["detail"] = message;
    document["active"] = statusName == "ACTIVE";
    document["source"] = "DI1";
    document["sensor"] = "Digital Input 1";
    document["channel"] = 1;
    document["di_channel"] = 1;
    document["di1_raw"] = readDI1Raw();
    document["di1_active"] = di1Active;
    document["di2_raw"] = readDI2Raw();
    document["di2_active"] = di2Active;
    document["relay_channel"] = RELAY_CH1_POWER;
    document["relay1"] = relay1On ? "ON" : "OFF";
    document["relay1_on"] = relay1On;
    document["relay5_sound"] = currentIndicatorState == INDICATOR_MAJOR_FAULT ? "ON" : "OFF";
    document["sound_active"] = currentIndicatorState == INDICATOR_MAJOR_FAULT;
    document["alarm_active"] = alarmActive;
    document["restore_pending"] = relayRestorePending;
    document["uptime_ms"] = millis();
    document["uptime_seconds"] = millis() / 1000;
    document["rtc_available"] = rtcAvailable;
    document["rtc_status"] = rtcStatusText();
    document["rtc_time"] = rtcTimestamp();
    document["rtc_set_from_build_time"] = rtcSetFromBuildTime;
    document["free_heap"] = ESP.getFreeHeap();
    document["wifi"] = WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED";

    if (WiFi.status() == WL_CONNECTED)
    {
        document["esp32_ip"] = WiFi.localIP().toString();
        document["ip"] = WiFi.localIP().toString();
    }

    String payload;
    serializeJson(document, payload);

    return payload;
}

bool sendEventToRender(
    const String &eventName,
    const String &statusName,
    const String &message)
{
    if (!COMMAND_SERVER_ENABLED || strlen(COMMAND_SERVER_BASE_URL) == 0)
    {
        Serial.println("[SERVER] Command server disabled");
        return false;
    }

    if (!connectWiFi())
    {
        Serial.println("[SERVER] Wi-Fi not connected");
        return false;
    }

    String url = COMMAND_SERVER_BASE_URL;
    url += "/api/alert";

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;

    Serial.print("[SERVER] POST ");
    Serial.println(url);

    if (!httpBeginForURL(http, plainClient, secureClient, url))
    {
        Serial.println("[SERVER] http.begin failed");
        return false;
    }

    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");

    const String payload = buildServerEventPayload(
        eventName,
        statusName,
        message
    );

    Serial.print("[SERVER] Event=");
    Serial.println(eventName);

    const int httpCode = http.POST(payload);
    const String response = http.getString();

    Serial.print("[SERVER] HTTP=");
    Serial.println(httpCode);

    if (response.length() > 0)
    {
        Serial.print("[SERVER] Response=");
        Serial.println(response);
    }

    http.end();

    return httpCode >= 200 && httpCode < 300;
}

bool isImportantLineEvent(const String &eventName)
{
    return eventName == "FAULT" ||
           eventName == "ALARM" ||
           eventName == "TRIP" ||
           eventName == "RECOVERY" ||
           eventName == "RESET" ||
           eventName == "WIFI_RESET" ||
           eventName == "BOOT";
}

void queueServerMessage(
    const String &eventName,
    const String &statusName,
    const String &message)
{
    if (serverMessagePending &&
        isImportantLineEvent(pendingEvent) &&
        !isImportantLineEvent(eventName))
    {
        Serial.print("[SERVER] Drop non-critical event while important event pending: ");
        Serial.println(eventName);
        return;
    }

    if (serverMessagePending)
    {
        Serial.print("[SERVER] Replace pending event ");
        Serial.print(pendingEvent);
        Serial.print(" with ");
        Serial.println(eventName);
    }

    pendingEvent = eventName;
    pendingStatus = statusName;
    pendingMessage = message;

    serverMessagePending = true;
    nextServerAttemptTime = millis();

    Serial.print("[SERVER] Queued event: ");
    Serial.println(eventName);

    updateStatusIndicators("line_queue_pending");
}

void updateServerQueue()
{
    if (!serverMessagePending)
    {
        return;
    }

    if (static_cast<int32_t>(
            millis() - nextServerAttemptTime) < 0)
    {
        return;
    }

    Serial.println(
        "[SERVER] Sending queued event..."
    );

    if (sendEventToRender(
            pendingEvent,
            pendingStatus,
            pendingMessage))
    {
        serverMessagePending = false;

        pendingEvent = "";
        pendingStatus = "";
        pendingMessage = "";

        Serial.println("[SERVER] Queue cleared");
        updateStatusIndicators("line_queue_cleared");
    }
    else
    {
        nextServerAttemptTime =
            millis() + SERVER_RETRY_DELAY_MS;

        Serial.println(
            "[SERVER] Send failed; "
            "retry in 30 seconds"
        );
        updateStatusIndicators("line_queue_retry");
    }
}

// =====================================================
// Digital Inputs
// =====================================================

int readDI1Raw()
{
    return digitalRead(DI1_PIN);
}

bool readDI1()
{
    return readDI1Raw() == DI1_ACTIVE_LEVEL;
}

int readDI2Raw()
{
    return digitalRead(DI2_PIN);
}

bool readDI2()
{
    return readDI2Raw() == DI2_ACTIVE_LEVEL;
}

const char *di1StateText(bool active)
{
    return active ? "ACTIVE" : "NORMAL";
}

void printInputDetail(
    const char *prefix,
    uint8_t pin,
    int rawValue,
    uint8_t activeLevel)
{
    const bool active = rawValue == activeLevel;

    Serial.print(prefix);
    Serial.print(" GPIO");
    Serial.print(pin);
    Serial.print(" RAW=");
    Serial.print(rawValue);
    Serial.print(" STATUS=");
    Serial.println(di1StateText(active));
}

void printDI1Detail(const char *prefix)
{
    printInputDetail(
        prefix,
        DI1_PIN,
        readDI1Raw(),
        DI1_ACTIVE_LEVEL
    );
}

void printDI2Detail(const char *prefix)
{
    printInputDetail(
        prefix,
        DI2_PIN,
        readDI2Raw(),
        DI2_ACTIVE_LEVEL
    );
}


void startRelayRestoreDelay(const char *reason)
{
    if (relayRestorePending)
    {
        return;
    }

    relayRestorePending = true;
    relayRestoreStartTime = millis();

    Serial.print("[SYSTEM] Relay CH1 restore pending for 30 seconds reason=");
    Serial.println(reason);

    updateStatusIndicators("restore_pending");
}

void activateDI1Alarm()
{
    relayRestorePending = false;

    if (alarmActive)
    {
        updateStatusIndicators("alarm_active");
        return;
    }

    alarmActive = true;

    Serial.println();
    Serial.println(
        "======================================"
    );
    Serial.println(
        "[ALARM] DIGITAL INPUT 1 ACTIVE"
    );
    Serial.println(
        "[ALARM] Cutting Relay CH1"
    );
    Serial.println(
        "======================================"
    );

    // ตัดรีเลย์ก่อนทำงานด้านเครือข่าย
    setRelay(RELAY_CH1_POWER, false);

    queueServerMessage(
        "FAULT",
        "ACTIVE",
        "ตรวจพบสัญญาณผิดปกติจาก Digital Input 1\n"
        "Relay CH1 ถูกสั่ง OFF\n"
        "ระบบถูกตัดเพื่อความปลอดภัย"
    );

    updateStatusIndicators("di1_alarm");
}

void updateDI1()
{
    const bool activeState = readDI1();

    if (activeState != di1LastActiveState)
    {
        di1LastActiveState = activeState;
        di1LastChangeTime = millis();
    }

    if (millis() - di1LastChangeTime <
        DI_DEBOUNCE_MS)
    {
        return;
    }

    if (activeState == di1Active)
    {
        return;
    }

    di1Active = activeState;

    Serial.print("[DI1] ");
    Serial.println(
        di1StateText(di1Active)
    );

    if (di1Active)
    {
        activateDI1Alarm();
    }
    else
    {
        Serial.println(
            "[DI1] Signal returned to normal"
        );
        startRelayRestoreDelay("di1_normal");
    }
}

void updateDI2()
{
    const bool activeState = readDI2();

    if (activeState != di2LastActiveState)
    {
        di2LastActiveState = activeState;
        di2LastChangeTime = millis();
    }

    if (millis() - di2LastChangeTime <
        DI_DEBOUNCE_MS)
    {
        return;
    }

    if (activeState == di2Active)
    {
        return;
    }

    di2Active = activeState;

    Serial.print("[DI2] ");
    Serial.println(
        di1StateText(di2Active)
    );

    updateStatusIndicators(
        di2Active ? "di2_minor_fault" : "di2_normal"
    );
}

void updateAutoRestore()
{
    if (!relayRestorePending)
    {
        if (alarmActive && !relay1On && !di1Active && !readDI1())
        {
            startRelayRestoreDelay("alarm_waiting_with_di1_normal");
        }
        else
        {
            return;
        }
    }

    if (di1Active || readDI1())
    {
        relayRestorePending = false;
        Serial.println(
            "[SYSTEM] Auto restore canceled: DI1 active again"
        );
        return;
    }

    if (millis() - relayRestoreStartTime <
        RELAY_RESTORE_DELAY_MS)
    {
        return;
    }

    relayRestorePending = false;
    alarmActive = false;

    Serial.println(
        "[SYSTEM] Restore delay complete"
    );
    Serial.println(
        "[SYSTEM] Auto restoring Relay CH1"
    );

    const String recoveryStamp =
        String("\nUptime: ") + String(millis() / 1000) +
        " วินาที";

    if (setRelay(RELAY_CH1_POWER, true))
    {
        Serial.println(
            "[SERVER] Queuing RECOVERY after restore delay"
        );
        const String recoveryMessage =
            String("Digital Input 1 กลับสู่สถานะปกติครบ 30 วินาทีแล้ว\n") +
            "Relay CH1 ถูกสั่ง ON อัตโนมัติ\n" +
            "ระบบกลับมาทำงานตามปกติ" +
            recoveryStamp;

        queueServerMessage(
            "RECOVERY",
            "NORMAL",
            recoveryMessage
        );
    }
    else
    {
        const String recoveryMessage =
            String("Digital Input 1 กลับสู่สถานะปกติครบ 30 วินาทีแล้ว\n") +
            "แต่สั่ง Relay CH1 ON ไม่สำเร็จ\n" +
            "กรุณาตรวจสอบ Relay Controller" +
            recoveryStamp;

        queueServerMessage(
            "RECOVERY",
            "NORMAL",
            recoveryMessage
        );
    }
}

// =====================================================
// Status
// =====================================================

void showStatus()
{
    Serial.println();
    Serial.println(
        "========== SYSTEM STATUS =========="
    );

    Serial.print("Wi-Fi       : ");
    Serial.println(
        WiFi.status() == WL_CONNECTED
            ? "CONNECTED"
            : "DISCONNECTED"
    );

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.print("ESP32 IP    : ");
        Serial.println(WiFi.localIP());
    }

    Serial.print("Command Srv : ");
    Serial.println(COMMAND_SERVER_BASE_URL);

    Serial.print("DI1 Logic   : NC NORMAL=0, ACTIVE=1");
    Serial.println();

    Serial.print("DI2 Logic   : NC NORMAL=0, ACTIVE=1");
    Serial.println();

    Serial.print("DI1 RAW     : ");
    Serial.println(readDI1Raw());

    Serial.print("DI1         : ");
    Serial.println(
        di1StateText(di1Active)
    );

    Serial.print("DI2 RAW     : ");
    Serial.println(readDI2Raw());

    Serial.print("DI2         : ");
    Serial.println(
        di1StateText(di2Active)
    );

    Serial.print("Relay CH1   : ");
    Serial.println(
        relay1On ? "ON" : "OFF"
    );

    Serial.print("Sound CH5   : ");
    Serial.println(
        currentIndicatorState == INDICATOR_MAJOR_FAULT ? "ON" : "OFF"
    );

    Serial.print("Alarm       : ");
    Serial.println(
        alarmActive
            ? "ACTIVE"
            : "NORMAL"
    );

    Serial.print("Restore     : ");
    Serial.println(
        relayRestorePending
            ? "PENDING"
            : "IDLE"
    );

    Serial.print("Status Light: ");
    Serial.println(
        indicatorStateText(currentIndicatorState)
    );

    Serial.print("LINE Queue  : ");
    Serial.println(
        serverMessagePending
            ? "PENDING"
            : "EMPTY"
    );

    Serial.print("Free Heap   : ");
    Serial.println(ESP.getFreeHeap());

    Serial.println(
        "==================================="
    );
}

// =====================================================
// Serial Commands
// =====================================================

void processCommand(String command)
{
    command.trim();
    command.toUpperCase();

    if (command.length() == 0)
    {
        return;
    }

    Serial.print("[COMMAND] ");
    Serial.println(command);

    if (command == "ON")
    {
        if (alarmActive || di1Active || readDI1())
        {
            Serial.println(
                "[ON] Refused: Alarm/DI1 active"
            );
            return;
        }

        if (setRelay(RELAY_CH1_POWER, true))
        {
            queueServerMessage(
                "RELAY_ON",
                "ACTIVE",
                "Relay CH1 ถูกสั่ง ON"
            );
        }
    }
    else if (command == "OFF")
    {
        if (setRelay(RELAY_CH1_POWER, false))
        {
            queueServerMessage(
                "RELAY_OFF",
                "ACTIVE",
                "Relay CH1 ถูกสั่ง OFF"
            );
        }
    }
    else if (command == "ALARM")
    {
        activateDI1Alarm();
    }
    else if (command == "RESET")
    {
        if (!alarmActive)
        {
            Serial.println(
                "[RESET] No active alarm"
            );
            return;
        }

        if (di1Active || readDI1())
        {
            Serial.println(
                "[RESET] Refused: "
                "DI1 still ACTIVE"
            );
            return;
        }

        alarmActive = false;

        Serial.println(
            "[RESET] Alarm cleared; restore delay started"
        );

        startRelayRestoreDelay("manual_reset");
        queueServerMessage(
            "RESET",
            "NORMAL",
            "Alarm ถูกรีเซ็ตแล้ว\n"
            "ระบบจะรอ 30 วินาทีก่อนเปิด Relay CH1 อัตโนมัติ"
        );
    }
    else if (command == "TEST")
    {
        queueServerMessage(
            "TEST",
            "ACTIVE",
            "ตรวจสอบการเชื่อมต่อ LINE สำเร็จ"
        );

        Serial.println(
            "[TEST] Server message queued"
        );
    }
    else if (command == "STATUS")
    {
        showStatus();
    }
    else if (command == "CHECK")
    {
        showStatus();
        queueServerMessage(
            "SYSTEM_CHECK",
            "NORMAL",
            buildSystemCheckMessage()
        );
    }
    else if (command == "WIFI_RESET")
    {
        resetWiFiSettings();
    }
    else if (command == "RAW")
    {
        printDI1Detail("[DI1]");
        printDI2Detail("[DI2]");
    }
    else if (command == "HELP")
    {
        Serial.println();
        Serial.println("Commands:");
        Serial.println(
            "ON     - เปิด Relay CH1"
        );
        Serial.println(
            "OFF    - ปิด Relay CH1"
        );
        Serial.println(
            "ALARM  - จำลอง Alarm"
        );
        Serial.println(
            "RESET  - รีเซ็ตระบบ"
        );
        Serial.println(
            "TEST   - ตรวจสอบส่ง LINE"
        );
        Serial.println(
            "STATUS - แสดงสถานะ"
        );
        Serial.println(
            "CHECK  - ตรวจระบบทั้งหมด"
        );
        Serial.println(
            "WIFI_RESET - ล้างค่า Wi-Fi"
        );
        Serial.println(
            "RAW    - อ่านค่าดิบ DI1/DI2"
        );
        Serial.println(
            "HELP   - แสดงคำสั่ง"
        );
    }
    else
    {
        Serial.println(
            "[COMMAND] Unknown command"
        );
        Serial.println("Type HELP");
    }
}

String buildStatusMessage()
{
    String message;
    message += "สถานะระบบล่าสุด\n";
    message += "Wi-Fi: ";
    message += WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED";
    message += "\nDI1 RAW: " + String(readDI1Raw());
    message += "\nDI1: " + String(di1StateText(di1Active));
    message += "\nDI2 RAW: " + String(readDI2Raw());
    message += "\nDI2: " + String(di1StateText(di2Active));
    message += "\nRelay CH1: ";
    message += relay1On ? "ON" : "OFF";
    message += "\nSound CH5: ";
    message += currentIndicatorState == INDICATOR_MAJOR_FAULT ? "ON" : "OFF";
    message += "\nAlarm: ";
    message += alarmActive ? "ACTIVE" : "NORMAL";
    message += "\nRestore: ";
    message += relayRestorePending ? "PENDING" : "IDLE";
    message += "\nStatus Light: ";
    message += indicatorStateText(currentIndicatorState);
    message += "\nLINE Queue: ";
    message += serverMessagePending ? "PENDING" : "EMPTY";
    message += "\nRTC: " + rtcStatusText();
    message += "\nเวลา RTC: " + rtcTimestamp();
    message += "\nUptime: " + String(millis() / 1000) + " วินาที";

    return message;
}

bool checkCommandServer()
{
    if (!COMMAND_SERVER_ENABLED || strlen(COMMAND_SERVER_BASE_URL) == 0)
    {
        return false;
    }

    if (!connectWiFi())
    {
        return false;
    }

    String url = COMMAND_SERVER_BASE_URL;
    url += "/health";

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;

    if (!httpBeginForURL(http, plainClient, secureClient, url))
    {
        return false;
    }

    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Connection", "close");

    const int code = http.GET();
    http.end();

    return code >= 200 && code < 300;
}

String okText(bool ok)
{
    return ok ? "OK" : "NG";
}

String buildSystemCheckMessage()
{
    const bool wifiOk = WiFi.status() == WL_CONNECTED || connectWiFi();
    const bool relayControllerOk = isRelayControllerOnline();
    const bool commandServerOk = checkCommandServer();
    const bool di1RawNormal = readDI1Raw() == LOW;
    const bool di1StateNormal = !di1Active && !readDI1();
    const bool di2RawNormal = readDI2Raw() == LOW;
    const bool di2StateNormal = !di2Active && !readDI2();
    const bool expectedRelayOn = !alarmActive && !di1Active && !readDI1() && !relayRestorePending;
    const bool relayStateOk = relay1On == expectedRelayOn;
    const bool lineQueueOk = !serverMessagePending;
    const bool heapOk = ESP.getFreeHeap() > 50000;
    const bool restoreOk = !relayRestorePending || (!di1Active && !readDI1());
    const bool allOk = wifiOk && relayControllerOk && commandServerOk &&
                       di1StateNormal && di2StateNormal &&
                       relayStateOk && lineQueueOk &&
                       heapOk && restoreOk;

    String message;
    message += "ตรวจระบบทั้งหมด\n";
    message += "ผลรวม: ";
    message += allOk ? "ปกติ" : "ต้องตรวจสอบ";
    message += "\nWi-Fi: " + okText(wifiOk);

    if (wifiOk)
    {
        message += "\nIP: " + WiFi.localIP().toString();
    }

    message += "\nRender Server: " + okText(commandServerOk);
    message += "\nRelay Controller: " + okText(relayControllerOk);
    message += "\nDI1 RAW: " + String(readDI1Raw());
    message += di1RawNormal ? " (ปกติ NC ปิด)" : " (ทำงาน/วงจรเปิด)";
    message += "\nDI1 State: ";
    message += di1StateNormal ? "NORMAL" : "ACTIVE";
    message += "\nDI2 RAW: " + String(readDI2Raw());
    message += di2RawNormal ? " (ปกติ NC ปิด)" : " (ทำงาน/วงจรเปิด)";
    message += "\nDI2 State: ";
    message += di2StateNormal ? "NORMAL" : "ACTIVE";
    message += "\nRelay CH1: ";
    message += relay1On ? "ON" : "OFF";
    message += "\nSound CH5: ";
    message += currentIndicatorState == INDICATOR_MAJOR_FAULT ? "ON" : "OFF";
    message += "\nRelay Logic: " + okText(relayStateOk);
    message += "\nAlarm: ";
    message += alarmActive ? "ACTIVE" : "NORMAL";
    message += "\nRestore: ";
    message += relayRestorePending ? "PENDING" : "IDLE";
    message += "\nLINE Queue: ";
    message += serverMessagePending ? "PENDING" : "EMPTY";
    message += "\nStatus Light: ";
    message += indicatorStateText(currentIndicatorState);
    message += "\nRTC: " + rtcStatusText();
    message += "\nเวลา RTC: " + rtcTimestamp();
    message += "\nFree Heap: " + String(ESP.getFreeHeap());

    if (!allOk)
    {
        message += "\nหมายเหตุ: DI1 เป็น alarm สีแดงพร้อมเสียง CH5, DI2 เป็น dry contact NC สีเหลือง";
    }

    return message;
}

bool executeRemoteCommand(const String &command)
{
    Serial.print("[REMOTE-COMMAND] ");
    Serial.println(command);

    if (command == "STATUS")
    {
        showStatus();
        queueServerMessage(
            "STATUS",
            "NORMAL",
            buildStatusMessage()
        );
        return true;
    }

    if (command == "CHECK")
    {
        showStatus();
        queueServerMessage(
            "SYSTEM_CHECK",
            "NORMAL",
            buildSystemCheckMessage()
        );
        return true;
    }

    if (command == "WIFI_RESET")
    {
        resetWiFiSettings();
        return true;
    }

    if (command == "TEST")
    {
        queueServerMessage(
            "TEST",
            "ACTIVE",
            "ตรวจสอบการเชื่อมต่อ LINE สำเร็จ"
        );
        return true;
    }

    if (command == "ON")
    {
        if (alarmActive || di1Active || readDI1())
        {
            queueServerMessage(
                "COMMAND_REFUSED",
                "ACTIVE",
                "คำสั่ง ON ถูกปฏิเสธ\nระบบยังมี Alarm หรือ DI1 ยังผิดปกติ"
            );
            return false;
        }

        if (setRelay(RELAY_CH1_POWER, true))
        {
            queueServerMessage(
                "RELAY_ON",
                "ACTIVE",
                "Relay CH1 ถูกสั่ง ON ผ่าน LINE"
            );
            return true;
        }

        queueServerMessage(
            "COMMAND_FAILED",
            "ACTIVE",
            "สั่ง Relay CH1 ON ผ่าน LINE ไม่สำเร็จ"
        );
        return false;
    }

    if (command == "OFF")
    {
        if (setRelay(RELAY_CH1_POWER, false))
        {
            queueServerMessage(
                "RELAY_OFF",
                "ACTIVE",
                "Relay CH1 ถูกสั่ง OFF ผ่าน LINE"
            );
            return true;
        }

        queueServerMessage(
            "COMMAND_FAILED",
            "ACTIVE",
            "สั่ง Relay CH1 OFF ผ่าน LINE ไม่สำเร็จ"
        );
        return false;
    }

    if (command == "RESET")
    {
        if (!alarmActive)
        {
            queueServerMessage(
                "RESET",
                "NORMAL",
                "ไม่มี Alarm ค้างอยู่\nระบบอยู่ในสถานะปกติแล้ว"
            );
            return true;
        }

        if (di1Active || readDI1())
        {
            queueServerMessage(
                "COMMAND_REFUSED",
                "ACTIVE",
                "คำสั่ง RESET ถูกปฏิเสธ\nDI1 ยังผิดปกติอยู่"
            );
            return false;
        }

        alarmActive = false;
        startRelayRestoreDelay("line_reset");

        queueServerMessage(
            "RESET",
            "NORMAL",
            "Alarm ถูกรีเซ็ตผ่าน LINE\nระบบจะรอ 30 วินาทีก่อนเปิด Relay CH1 อัตโนมัติ"
        );
        return true;
    }

    return false;
}

bool httpBeginForURL(HTTPClient &http, WiFiClient &plainClient, WiFiClientSecure &secureClient, const String &url)
{
    if (url.startsWith("https://"))
    {
        secureClient.setInsecure();
        return http.begin(secureClient, url);
    }

    return http.begin(plainClient, url);
}

void updateServerHeartbeat()
{
    if (!COMMAND_SERVER_ENABLED || strlen(COMMAND_SERVER_BASE_URL) == 0)
    {
        return;
    }

    if (millis() - lastServerHeartbeatTime < SERVER_HEARTBEAT_INTERVAL_MS)
    {
        return;
    }

    lastServerHeartbeatTime = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    String url = COMMAND_SERVER_BASE_URL;
    url += "/api/heartbeat";

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;

    if (!httpBeginForURL(http, plainClient, secureClient, url))
    {
        Serial.println("[HEARTBEAT] http.begin failed");
        return;
    }

    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");

    JsonDocument document;
    document["device"] = DEVICE_ID;
    document["controller"] = "Cason Solar Safety Controller";
    document["uptime_seconds"] = millis() / 1000;
    document["rtc_available"] = rtcAvailable;
    document["rtc_status"] = rtcStatusText();
    document["rtc_time"] = rtcTimestamp();
    document["rtc_set_from_build_time"] = rtcSetFromBuildTime;
    document["free_heap"] = ESP.getFreeHeap();
    document["di1_raw"] = readDI1Raw();
    document["di1_active"] = di1Active;
    document["di2_raw"] = readDI2Raw();
    document["di2_active"] = di2Active;
    document["relay1"] = relay1On ? "ON" : "OFF";
    document["relay1_on"] = relay1On;
    document["relay5_sound"] = currentIndicatorState == INDICATOR_MAJOR_FAULT ? "ON" : "OFF";
    document["sound_active"] = currentIndicatorState == INDICATOR_MAJOR_FAULT;
    document["alarm_active"] = alarmActive;
    document["restore_pending"] = relayRestorePending;
    document["line_queue_pending"] = serverMessagePending;
    document["wifi"] = "CONNECTED";
    document["ip"] = WiFi.localIP().toString();

    String payload;
    serializeJson(document, payload);

    const int code = http.POST(payload);
    Serial.print("[HEARTBEAT] HTTP=");
    Serial.println(code);

    http.end();
}


void postCommandResult(
    const String &commandId,
    const String &command,
    bool ok,
    const String &detail)
{
    if (!COMMAND_SERVER_ENABLED || strlen(COMMAND_SERVER_BASE_URL) == 0)
    {
        return;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    String url = COMMAND_SERVER_BASE_URL;
    url += "/api/command/result";

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;

    if (!httpBeginForURL(http, plainClient, secureClient, url))
    {
        Serial.println("[COMMAND-POLL] result http.begin failed");
        return;
    }

    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");

    JsonDocument document;
    document["id"] = commandId;
    document["command"] = command;
    document["ok"] = ok;
    document["detail"] = detail;
    document["device"] = DEVICE_ID;
    document["uptime_seconds"] = millis() / 1000;

    String payload;
    serializeJson(document, payload);

    const int code = http.POST(payload);
    Serial.print("[COMMAND-POLL] result HTTP=");
    Serial.println(code);

    http.end();
}

void updateCommandPoll()
{
    if (!COMMAND_SERVER_ENABLED || strlen(COMMAND_SERVER_BASE_URL) == 0)
    {
        return;
    }

    if (millis() - lastCommandPollTime < COMMAND_POLL_INTERVAL_MS)
    {
        return;
    }

    lastCommandPollTime = millis();

    if (!connectWiFi())
    {
        Serial.println("[COMMAND-POLL] Wi-Fi not connected");
        return;
    }

    String url = COMMAND_SERVER_BASE_URL;
    url += "/api/command?device=";
    url += DEVICE_ID;

    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    HTTPClient http;

    if (!httpBeginForURL(http, plainClient, secureClient, url))
    {
        Serial.println("[COMMAND-POLL] http.begin failed");
        return;
    }

    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Connection", "close");

    const int code = http.GET();
    const String response = http.getString();
    http.end();

    if (code < 200 || code >= 300)
    {
        Serial.print("[COMMAND-POLL] HTTP=");
        Serial.println(code);
        return;
    }

    JsonDocument document;
    DeserializationError error = deserializeJson(document, response);

    if (error)
    {
        Serial.print("[COMMAND-POLL] JSON error=");
        Serial.println(error.c_str());
        return;
    }

    const String command = String(document["command"] | "");

    if (command.length() == 0)
    {
        return;
    }

    const String commandId = String(document["id"] | "");
    const bool ok = executeRemoteCommand(command);

    postCommandResult(
        commandId,
        command,
        ok,
        ok ? "executed" : "refused_or_failed"
    );
}

void updateSerial()
{
    static String commandBuffer;

    while (Serial.available() > 0)
    {
        const char c =
            static_cast<char>(Serial.read());

        if (c == '\n' || c == '\r')
        {
            if (commandBuffer.length() > 0)
            {
                processCommand(commandBuffer);
                commandBuffer = "";
            }
        }
        else if (commandBuffer.length() < 64)
        {
            commandBuffer += c;
        }
        else
        {
            commandBuffer = "";

            Serial.println(
                "[COMMAND] Input too long; cleared"
            );
        }
    }
}

// =====================================================
// Heartbeat
// =====================================================

void updateHeartbeat()
{
    if (millis() - lastHeartbeatTime <
        HEARTBEAT_MS)
    {
        return;
    }

    lastHeartbeatTime = millis();

    Serial.print("[RUN] DI1_RAW=");
    Serial.print(readDI1Raw());

    Serial.print(" DI1=");
    Serial.print(
        di1StateText(di1Active)
    );

    Serial.print(" DI2_RAW=");
    Serial.print(readDI2Raw());

    Serial.print(" DI2=");
    Serial.print(
        di1StateText(di2Active)
    );

    Serial.print(" ALARM=");
    Serial.print(
        alarmActive ? "ACTIVE" : "NORMAL"
    );

    Serial.print(" RESTORE=");
    Serial.print(
        relayRestorePending ? "PENDING" : "IDLE"
    );

    Serial.print(" WIFI=");
    Serial.print(
        WiFi.status() == WL_CONNECTED
            ? "CONNECTED"
            : "DISCONNECTED"
    );

    Serial.print(" LINE_QUEUE=");
    Serial.println(
        serverMessagePending
            ? "PENDING"
            : "EMPTY"
    );
}

// =====================================================
// Setup
// =====================================================

void setup()
{
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    Serial.println(
        "======================================"
    );
    Serial.println(
        "   CASON SOLAR SAFETY CONTROLLER"
    );
    Serial.println(
        "     ESP32 -> RENDER -> LINE"
    );
    Serial.println(
        "======================================"
    );

    // DI1 เป็น dry contact NC: ปกติปิด = LOW (0), ทำงาน/เปิดวงจร = HIGH (1)
    // DI2 เป็น dry contact NC: ปกติปิด = LOW (0), ทำงาน/เปิดวงจร = HIGH (1)
    pinMode(DI1_PIN, DI1_INPUT_MODE);
    pinMode(DI2_PIN, DI2_INPUT_MODE);

    di1LastActiveState = readDI1();
    di1Active = di1LastActiveState;
    di1LastChangeTime = millis();

    di2LastActiveState = readDI2();
    di2Active = di2LastActiveState;
    di2LastChangeTime = millis();

    printDI1Detail("[DI1] Startup");
    printDI2Detail("[DI2] Startup");

    if (!tcaBegin())
    {
        Serial.println(
            "[SYSTEM] Relay controller FAILED"
        );

        // หยุดอยู่ตรงนี้เพื่อความปลอดภัย
        while (true)
        {
            updateSerial();
            delay(100);
        }
    }

    rtcBegin();

    // Fail-safe:
    // ถ้า DI1 NC ปิดปกติ ให้เปิด Relay CH1
    // ถ้า DI1 เปิดวงจร/ผิดปกติตั้งแต่เปิดเครื่อง ให้ Relay CH1 คง OFF
    if (di1Active)
    {
        alarmActive = true;
        setRelay(RELAY_CH1_POWER, false);

        Serial.println();
        Serial.println(
            "======================================"
        );
        Serial.println(
            "[ALARM] DI1 ACTIVE AT STARTUP"
        );
        Serial.println(
            "[ALARM] Relay CH1 remains OFF"
        );
        Serial.println(
            "======================================"
        );
    }
    else
    {
        alarmActive = false;

        if (!setRelay(RELAY_CH1_POWER, true))
        {
            Serial.println(
                "[SYSTEM] Failed to turn Relay CH1 ON"
            );
        }
    }

    const bool wifiConnected = connectWiFi();

    if (wifiConnected)
    {
        Serial.println(
            "[SYSTEM] Wi-Fi ready"
        );
    }
    else
    {
        Serial.println(
            "[SYSTEM] Wi-Fi not ready"
        );
        Serial.println(
            "[SYSTEM] Server queue will retry"
        );
    }

    // ส่งเหตุการณ์ตามสถานะจริงตอนเปิดเครื่อง
    if (di1Active)
    {
        queueServerMessage(
            "FAULT",
            "ACTIVE",
            "ตรวจพบ Digital Input 1 ผิดปกติตั้งแต่เปิดเครื่อง\n"
            "Relay CH1 คงสถานะ OFF\n"
            "ระบบถูกล็อกเพื่อความปลอดภัย"
        );
    }
    else
    {
        queueServerMessage(
            "BOOT",
            "NORMAL",
            "CASON Solar Safety Controller\n"
            "ESP32 เปิดเครื่องเรียบร้อย\n"
            "Digital Input 1 ปกติ\n"
            "Digital Input 2 NC ปกติ\n"
            "Relay CH1 ถูกสั่ง ON\n"
            "ระบบกำลังทำงาน"
        );
    }

    Serial.println();
    Serial.println("Ready.");
    Serial.println(
        "Type: TEST, ON, OFF, ALARM, RESET, "
        "STATUS, CHECK, WIFI_RESET, RAW, HELP"
    );
}

// =====================================================
// Loop
// =====================================================

void loop()
{
    updateSerial();
    updateDI1();
    updateDI2();
    updateAutoRestore();
    updateStatusIndicators("loop");
    updateWiFiPortal();
    updateCommandPoll();
    updateServerHeartbeat();
    updateHeartbeat();
    updateServerQueue();
    updateWiFiReset();

    delay(5);
}
