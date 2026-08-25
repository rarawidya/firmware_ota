#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>
#include "stm32_uart_bootloader.h"

#define PIN_UART_TX 6   // ESP32 -> STM32 RX
#define PIN_UART_RX 7   // ESP32 <- STM32 TX
#define PIN_STM32_BOOT0 4
#define PIN_STM32_NRST 5
#define OTA_BUTTON_PIN 3
#define STATUS_LED_PIN 2

#define STM32_BAUD 115200

const char *FIRM_FILE = "/firmware.bin";
const char *PREF_NS = "esp_cfg";

typedef struct {
  bool gFlashRunning;
  bool gFlashDone;
  bool gFlashSuccess;
  int  gFlashPercent;
} FlashStatus_t;
FlashStatus_t gFlashStatus = {false, false, false, 0};

String deviceName, apPasswd, firmwareURL;
bool waiting;

HardwareSerial SerialSTM(1);
STM32Bootloader stmboot(SerialSTM, 1500);
WebServer server(80);
HTTPClient httpClient;
WiFiClient sseClient;
Preferences prefs;

TaskHandle_t StatusLEDTask_Handle;
TaskHandle_t FlashStatusTask_Handle;
TimerHandle_t otaButtonTimer_Handle;

static void stm_set_boot0(bool high) { digitalWrite(PIN_STM32_BOOT0, high ? HIGH : LOW); }
static void stm_reset_pulse() { digitalWrite(PIN_STM32_NRST, LOW); delay(50); digitalWrite(PIN_STM32_NRST, HIGH); delay(50); }

static void enter_bootloader_hw() {
  pinMode(PIN_STM32_BOOT0, OUTPUT);
  pinMode(PIN_STM32_NRST, OUTPUT);
  stm_set_boot0(true);
  delay(10);
  stm_reset_pulse();
  delay(120);
}

static void exit_bootloader_hw() {
  stm_set_boot0(false);
  delay(10);
  stm_reset_pulse();
  delay(50);
}

void handleLogEvents() {
  sseClient = server.client();

  // Send raw HTTP response manually
  sseClient.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "\r\n"
  );
}

void sendSSE(const String &msg) {
  if (!sseClient || !sseClient.connected()) return;

  sseClient.print("data: " + msg + "\n\n");
  sseClient.clear();
}

void flashTask(void *param) {
  bool ok = false;

  File f = SPIFFS.open(FIRM_FILE, FILE_READ);
  if (!f) {
    sendSSE("Failed to open firmware file");
    gFlashStatus.gFlashSuccess = false;
    gFlashStatus.gFlashDone    = true;
    gFlashStatus.gFlashRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  size_t total     = f.size();
  size_t remaining = total;

  sendSSE("=== STM32 Flash sequence started ===");
  enter_bootloader_hw();

  SerialSTM.begin(STM32_BAUD, SERIAL_8E1, PIN_UART_RX, PIN_UART_TX);
  delay(50);
  stmboot.setTimeout(1800);

  bool  localOk = false;
  const int tries = 3;
  int i;
  for (i = 0; i < tries; ++i) {
    sendSSE("Sync attempt " + String(i + 1) + "...");
    if (stmboot.begin()) break;
    delay(200);
  }

  const size_t CHUNK = 256;
  uint8_t buf[CHUNK];
  uint32_t addr = 0x08000000UL;

  if (i == tries) {
    sendSSE("Bootloader sync failed");
    goto finish;
  }
  sendSSE("Bootloader sync OK");

  if (!stmboot.eraseFull()) {
    sendSSE("Erase failed");
    goto finish;
  }
  sendSSE("Erase OK");

  sendSSE("Firmware size: " + String(total) + " bytes");

  while (remaining > 0) {
    size_t toRead = remaining > CHUNK ? CHUNK : remaining;
    size_t r      = f.read(buf, toRead);
    if (r == 0) {
      sendSSE("File read error");
      goto finish;
    }
    if (!stmboot.writeMemory(addr, buf, (uint16_t)r)) {
      sendSSE("Write failed at 0x" + String(addr, HEX));
      goto finish;
    }
    addr      += r;
    remaining -= r;
    sendSSE("Wrote " + String(r) + " bytes at 0x" + String(addr, HEX));

    size_t written = total - remaining;
    if (total > 0) {
      gFlashStatus.gFlashPercent = (int)((written * 100) / total);
    }

    delay(5);
  }

  if (!stmboot.go(0x08000000UL)) {
    sendSSE("GO failed");
    goto finish;
  }
  sendSSE("GO OK");
  localOk = true;

finish:
  f.close();
  SerialSTM.end();
  exit_bootloader_hw();
  sendSSE(String("Flashing finished: ") + (localOk ? "OK" : "FAILED"));
  sendSSE("=== STM32 Flash sequence ended ===");

  if (localOk) gFlashStatus.gFlashPercent = 100;

  gFlashStatus.gFlashSuccess = localOk;
  gFlashStatus.gFlashDone    = true;
  gFlashStatus.gFlashRunning = false;

  vTaskDelete(nullptr);  
}

void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  WiFi.reconnect();
  digitalWrite(STATUS_LED_PIN, HIGH);
  delay(100);
  digitalWrite(STATUS_LED_PIN, LOW);
  delay(100);
}

// ---------- Serve static files from SPIFFS ----------
void handleRoot() {
  if(WiFi.getMode() == WIFI_AP) {
    if (SPIFFS.exists("/index_ap.html")) {
      File f = SPIFFS.open("/index_ap.html", "r");
      server.streamFile(f, "text/html");
      f.close();
    } else {
      server.send(404, "text/plain", "index_ap.html not found in SPIFFS");
    }
  } else {
    if (SPIFFS.exists("/index_sta.html")) {
      File f = SPIFFS.open("/index_sta.html", "r");
      server.streamFile(f, "text/html");
      f.close();
    } else {
      server.send(404, "text/plain", "index_sta.html not found in SPIFFS");
    }
  }
}

void handleUpdateSTA() {
  if(server.arg("plain").length() == 0) {
    server.send(400, "text/plain", "Empty body");
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if(err) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  if(doc["staSsid"].is<const char*>() && doc["staPass"].is<const char*>()) {
    server.send(200, "text/plain", "Settings saved");
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.onEvent(WiFiStationDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.begin(doc["staSsid"].as<const char*>(), doc["staPass"].as<const char*>());
    vTaskSuspend(StatusLEDTask_Handle);
    digitalWrite(STATUS_LED_PIN, HIGH);
  }
}

void handleSettingsPage() {
  if (SPIFFS.exists("/setting.html")) {
    File f = SPIFFS.open("/setting.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  } else {
    server.send(404, "text/plain", "setting.html not found in SPIFFS");
  }
}

void handleCheckFirmware() {
  httpClient.begin("https://iqbiudnwcvcorliisbjk.supabase.co/functions/v1/swift-api");
  int httpCode = httpClient.GET();
  if (httpCode != HTTP_CODE_OK) {
    server.send(500, "text/plain", "Failed to check firmware");
    httpClient.end();
    return;
  }

  String response = httpClient.getString();
  server.send(200, "application/json", response);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  httpClient.end();
  if (err) {
    server.send(500, "text/plain", "Invalid JSON from firmware server");
    return;
  }
  firmwareURL = doc["firmware_url"].as<const char*>();
}

// ---------- Flash trigger endpoint ----------
void handleFlashTrigger() {
  if (gFlashStatus.gFlashRunning) {
    server.send(409, "text/plain", "Flashing already in progress");
    return;
  }

  httpClient.begin(firmwareURL);
  int httpCode = httpClient.GET();
  if (httpCode != HTTP_CODE_OK) {
    server.send(500, "text/plain", "Failed to download firmware");
    httpClient.end();
    return;
  }

  File f = SPIFFS.open(FIRM_FILE, "w");
  WiFiClient *stream = httpClient.getStreamPtr();
  while(httpClient.connected() && stream->available()) {
    f.write(stream->read());
  }
  f.close();
  httpClient.end();

  if (!SPIFFS.exists(FIRM_FILE)) {
    server.send(400, "text/plain", "No firmware found in SPIFFS");
    return;
  }

  gFlashStatus.gFlashRunning = true;
  gFlashStatus.gFlashDone    = false;
  gFlashStatus.gFlashSuccess = false;
  gFlashStatus.gFlashPercent = 0;

  xTaskCreatePinnedToCore(
      flashTask,
      "flashTask",
      8192,         
      nullptr,
      1,             
      &FlashStatusTask_Handle,
      0             
  );

  server.send(200, "text/plain", "Flashing started");
}

// ---------- Flash status endpoint ----------
void handleFlashStatus() {
  JsonDocument doc;
  doc["running"] = gFlashStatus.gFlashRunning;
  doc["done"]    = gFlashStatus.gFlashDone;
  doc["success"] = gFlashStatus.gFlashSuccess;
  doc["percent"] = gFlashStatus.gFlashPercent;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------- Settings endpoints ----------
void handleSettingsGet() {
  JsonDocument doc;
  doc["deviceName"]     = prefs.getString("device_name", "ESP32-OTA");
  doc["apPass"]     = prefs.getString("ap_pass", "esp32pass");
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSettingsPost() {
  if (server.arg("plain").length() == 0) {
    server.send(400, "text/plain", "Empty body");
    return;
  }
  String body = server.arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  if (doc["deviceName"].is<const char*>())     prefs.putString("device_name", doc["deviceName"].as<const char*>());
  if (doc["apPass"].is<const char*>())     prefs.putString("ap_pass", doc["apPass"].as<const char*>());

  server.send(200, "text/plain", "Settings saved");
}

void StatusLEDTask(void *pvParameters) {
  for(;;) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    digitalWrite(STATUS_LED_PIN, LOW);
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

void otaButtonTimerCallback(TimerHandle_t xTimer) {
  waiting = false;
  xTaskCreate(StatusLEDTask, "StatusLEDTask", 2048, NULL, 1, &StatusLEDTask_Handle);
}

void setup() {
  pinMode(OTA_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  while(!SPIFFS.begin(true)){
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(50);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(50);
  }
  otaButtonTimer_Handle = xTimerCreate("otaButtonTimer", 5000 / portTICK_PERIOD_MS, pdFALSE, (void *)0, otaButtonTimerCallback);
  xTimerStart(otaButtonTimer_Handle, portMAX_DELAY);
  waiting = true;
  digitalWrite(STATUS_LED_PIN, HIGH);
  delay(50);
  digitalWrite(STATUS_LED_PIN, LOW);

  while(waiting) {
    if(digitalRead(OTA_BUTTON_PIN)) {
      xTimerStop(otaButtonTimer_Handle, portMAX_DELAY);
      for(uint8_t i = 0; i < 3; i++) {
        digitalWrite(STATUS_LED_PIN, HIGH);
        delay(20);
        digitalWrite(STATUS_LED_PIN, LOW);
        delay(20);
      }
      esp_deep_sleep_enable_gpio_wakeup((1ULL << OTA_BUTTON_PIN), ESP_GPIO_WAKEUP_GPIO_LOW);
      esp_deep_sleep_start();
    }
  }
  
  prefs.begin(PREF_NS, false);
  deviceName = prefs.getString("device_name", "ESP32-OTA");
  apPasswd = prefs.getString("ap_pass", "esp32pass");

  pinMode(PIN_STM32_BOOT0, OUTPUT);
  pinMode(PIN_STM32_NRST, OUTPUT);
  digitalWrite(PIN_STM32_BOOT0, LOW);
  digitalWrite(PIN_STM32_NRST, HIGH);
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(deviceName.c_str(), apPasswd.c_str());
  String mdnsName = deviceName;
  mdnsName.replace(" ", "-");
  MDNS.begin(mdnsName.c_str());

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/setting.html", HTTP_GET, handleSettingsPage);
  server.on("/update_sta", HTTP_POST, handleUpdateSTA);

  // Upload & flash endpoints
  server.on("/check_firmware", HTTP_GET, handleCheckFirmware);
  server.on("/flash", HTTP_POST, handleFlashTrigger);
  server.on("/flash_status", HTTP_GET, handleFlashStatus);
  server.on("/log_events", handleLogEvents);

  // Settings endpoints
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  
  server.begin();
}

void loop() {
  server.handleClient();
}
