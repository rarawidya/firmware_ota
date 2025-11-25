// main.cpp  -- synchronized implementation for index.html & setting.html
// Uses: WebServer (synchronous) + Preferences + SPIFFS
// Requires: ArduinoJson library (install via Library Manager)

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "stm32_uart_bootloader.h"

// -------- CONFIG: change pins to match your board --------
#define PIN_UART_TX 6   // ESP32 -> STM32 RX
#define PIN_UART_RX 7   // ESP32 <- STM32 TX

#define PIN_STM32_BOOT0 4
#define PIN_STM32_NRST  5

#define STM32_BAUD 115200
const char *FIRM_FILE = "/firmware.bin";

WebServer server(80);
Preferences prefs;
const char *PREF_NS = "esp_cfg";

String deviceName, wifiMode, staSSID, staPasswd, apSSID, apPasswd;

HardwareSerial SerialSTM(1);
STM32Bootloader stmboot(SerialSTM, 1500);

// ---- Flash progress state (for web UI polling) ----
volatile bool gFlashRunning = false;
volatile bool gFlashDone    = false;
volatile bool gFlashSuccess = false;
volatile int  gFlashPercent = 0;

TaskHandle_t gFlashTaskHandle = nullptr;

// Helper for hardware control (BOOT0/NRST)
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

// WiFi disconnected callback (simple reconnect)
void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println("WiFi disconnected, attempting reconnect...");
  WiFi.reconnect();
  delay(1000);
}

// Background task that actually flashes the STM32
void flashTask(void *param) {
  bool ok = false;

  File f = SPIFFS.open(FIRM_FILE, FILE_READ);
  if (!f) {
    Serial.println("Failed to open firmware file");
    gFlashSuccess = false;
    gFlashDone    = true;
    gFlashRunning = false;
    vTaskDelete(nullptr);
    return;
  }

  size_t total     = f.size();
  size_t remaining = total;

  Serial.println("=== STM32 Flash sequence started ===");
  enter_bootloader_hw();

  // Start UART with EVEN parity (8E1)
  SerialSTM.begin(STM32_BAUD, SERIAL_8E1, PIN_UART_RX, PIN_UART_TX);
  delay(50);
  stmboot.setTimeout(1800);

  bool  localOk = false;
  const int tries = 3;
  int i;
  for (i = 0; i < tries; ++i) {
    Serial.printf("Sync attempt %d...\n", i + 1);
    if (stmboot.begin()) break;
    delay(200);
  }

  const size_t CHUNK = 256;
  uint8_t buf[CHUNK];
  uint32_t addr = 0x08000000UL;

  if (i == tries) {
    Serial.println("Bootloader sync failed");
    goto finish;
  }
  Serial.println("Bootloader sync OK");

  if (!stmboot.eraseFull()) {
    Serial.println("Erase failed");
    goto finish;
  }
  Serial.println("Erase OK");

  Serial.printf("Firmware size: %u bytes\n", (unsigned)total);

  while (remaining > 0) {
    size_t toRead = remaining > CHUNK ? CHUNK : remaining;
    size_t r      = f.read(buf, toRead);
    if (r == 0) {
      Serial.println("File read error");
      goto finish;
    }
    if (!stmboot.writeMemory(addr, buf, (uint16_t)r)) {
      Serial.printf("Write failed at 0x%08X\n", addr);
      goto finish;
    }
    addr      += r;
    remaining -= r;
    Serial.printf("Wrote %u bytes to 0x%08X\n", (unsigned)r, (unsigned)(addr - r));

    // 🔥 Update percent based on bytes written
    size_t written = total - remaining;
    if (total > 0) {
      gFlashPercent = (int)((written * 100) / total);
    }

    delay(5);
  }

  if (!stmboot.go(0x08000000UL)) {
    Serial.println("GO failed");
    goto finish;
  }
  Serial.println("GO OK");
  localOk = true;

finish:
  f.close();
  SerialSTM.end();
  exit_bootloader_hw();
  Serial.printf("Flashing finished: %s\n", localOk ? "OK" : "FAIL");
  Serial.println("=== STM32 Flash sequence ended ===");

  // Final status
  if (localOk) {
    gFlashPercent = 100;
  }
  gFlashSuccess = localOk;
  gFlashDone    = true;
  gFlashRunning = false;

  vTaskDelete(nullptr);   // end this task
}

// ---------- Serve static files from SPIFFS ----------
// root -> /index.html
void handleRoot() {
  if (SPIFFS.exists("/index.html")) {
    File f = SPIFFS.open("/index.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  } else {
    server.send(404, "text/plain", "index.html not found in SPIFFS");
  }
}

// serve setting.html
void handleSettingsPage() {
  if (SPIFFS.exists("/setting.html")) {
    File f = SPIFFS.open("/setting.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  } else {
    server.send(404, "text/plain", "setting.html not found in SPIFFS");
  }
}

// ---------- Upload handlers (multipart/form-data) ----------
// Called when upload finished (endpoint completes)
void handleUploadDone() {
  server.send(200, "text/plain", "OK");
}

// Upload streaming callback (writes chunks to SPIFFS)
void handleFileUpload() {
  HTTPUpload &upload = server.upload();
  static File uploadFile;
  if (upload.status == UPLOAD_FILE_START) {
    if (SPIFFS.exists(FIRM_FILE)) SPIFFS.remove(FIRM_FILE);
    uploadFile = SPIFFS.open(FIRM_FILE, FILE_WRITE);
    if (!uploadFile) {
      Serial.println("Failed to open file for upload");
    } else {
      Serial.printf("Upload start: filename=%s\n", upload.filename.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      // upload.buf contains the received chunk
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload finished (%u bytes)\n", (unsigned)upload.totalSize);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
      SPIFFS.remove(FIRM_FILE);
      Serial.println("Upload aborted");
    }
  }
}

// ---------- Flash trigger endpoint (POST /flash) ----------
void handleFlashTrigger() {
  // Prevent double-start
  if (gFlashRunning) {
    server.send(409, "text/plain", "Flashing already in progress");
    return;
  }

  if (!SPIFFS.exists(FIRM_FILE)) {
    server.send(400, "text/plain", "No firmware found in SPIFFS");
    return;
  }

  // Reset state for new run
  gFlashRunning = true;
  gFlashDone    = false;
  gFlashSuccess = false;
  gFlashPercent = 0;

  // Start flash task on core 1 (for example)
  xTaskCreatePinnedToCore(
      flashTask,
      "flashTask",
      8192,          // stack size
      nullptr,
      1,             // priority
      &gFlashTaskHandle,
      0              // core
  );

  // Immediate reply so UI can start polling
  server.send(200, "text/plain", "Flashing started");
}

// ---------- Flash status endpoint (GET /flash_status) ----------
void handleFlashStatus() {
  StaticJsonDocument<128> doc;
  doc["running"] = gFlashRunning;
  doc["done"]    = gFlashDone;
  doc["success"] = gFlashSuccess;
  doc["percent"] = gFlashPercent;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}


// ---------- Settings endpoints ----------
// GET /settings  -> returns JSON with current prefs
void handleSettingsGet() {
  DynamicJsonDocument doc(512);
  doc["deviceName"] = prefs.getString("device_name", "STM32Flasher");
  doc["wifiMode"]   = prefs.getString("wifi_mode", "ap"); // 'ap','sta','apsta'
  doc["apSsid"]     = prefs.getString("ap_ssid", "ESP32-OTA");
  doc["apPass"]     = prefs.getString("ap_pass", "esp32pass");
  doc["staSsid"]    = prefs.getString("sta_ssid", "");
  doc["staPass"]    = prefs.getString("sta_pass", "");
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// POST /settings  -> accepts JSON { deviceName, wifiMode, apSsid, apPass, staSsid, staPass }
void handleSettingsPost() {
  if (server.arg("plain").length() == 0) {
    server.send(400, "text/plain", "Empty body");
    return;
  }
  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  if (doc.containsKey("deviceName")) prefs.putString("device_name", doc["deviceName"].as<const char*>());
  if (doc.containsKey("wifiMode"))   prefs.putString("wifi_mode", doc["wifiMode"].as<const char*>());
  if (doc.containsKey("apSsid"))     prefs.putString("ap_ssid", doc["apSsid"].as<const char*>());
  if (doc.containsKey("apPass"))     prefs.putString("ap_pass", doc["apPass"].as<const char*>());
  if (doc.containsKey("staSsid"))    prefs.putString("sta_ssid", doc["staSsid"].as<const char*>());
  if (doc.containsKey("staPass"))    prefs.putString("sta_pass", doc["staPass"].as<const char*>());

  server.send(200, "text/plain", "Settings saved");
  ESP.restart();
}

// ---------- AT command handling through USB Serial ----------
String serialLine = "";
void handleATCommand(const String &cmd) {
  String s = cmd; s.trim();
  if (s.length() == 0) return;

  Serial.printf("AT: %s\n", s.c_str());
  if (s.startsWith("AT+DEV_NAME")) {
    int sp = s.indexOf(' ');
    if (sp >= 0) prefs.putString("device_name", s.substring(sp+1));
    Serial.println("OK");
  } else if (s.startsWith("AT+WIFI_MODE")) {
    int sp = s.indexOf(' ');
    if (sp >= 0) prefs.putString("wifi_mode", s.substring(sp+1));
    Serial.println("OK");
  } else if (s.startsWith("AT+AP_SSID")) {
    int sp = s.indexOf(' ');
    if (sp >= 0) prefs.putString("ap_ssid", s.substring(sp+1));
    Serial.println("OK");
  } else if (s.startsWith("AT+AP_PASSWD")) {
    int sp = s.indexOf(' ');
    if (sp >= 0) prefs.putString("ap_pass", s.substring(sp+1));
    Serial.println("OK");
  } else if (s.startsWith("AT+STA_SSID")) {
    int sp = s.indexOf(' ');
    if (sp >= 0) prefs.putString("sta_ssid", s.substring(sp+1));
    Serial.println("OK");
  } else if (s.startsWith("AT+STA_PASSWD")) {
    int sp = s.indexOf(' ');
    if (sp >= 0) prefs.putString("sta_pass", s.substring(sp+1));
    Serial.println("OK");
  } else if (s == "AT+FLASH") {
    Serial.println("Starting flash...");
    handleFlashTrigger();
  } else if (s == "AT+SHOW") {
    Serial.printf("device_name=%s\n", prefs.getString("device_name", "STM32Flasher").c_str());
    Serial.printf("wifi_mode=%s\n", prefs.getString("wifi_mode", "ap").c_str());
    Serial.printf("ap_ssid=%s\n", prefs.getString("ap_ssid","ESP32-OTA").c_str());
    Serial.printf("sta_ssid=%s\n", prefs.getString("sta_ssid","").c_str());
  } else {
    Serial.println("UNKNOWN");
  }
}

void checkSerialInput() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (serialLine.length()) {
        handleATCommand(serialLine);
        serialLine = "";
      }
    } else {
      serialLine += c;
      if (serialLine.length() > 300) serialLine = "";
    }
  }
}

// ---------- setup and loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32 STM32 OTA Bridge (synchronized)");

  // mount SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
    while (1) delay(1000);
  }

  // open Preferences
  prefs.begin(PREF_NS, false);

  // load defaults
  deviceName = prefs.getString("device_name", "STM32Flasher");
  wifiMode   = prefs.getString("wifi_mode", "ap");
  staSSID    = prefs.getString("sta_ssid", "");
  staPasswd  = prefs.getString("sta_pass", "");
  apSSID     = prefs.getString("ap_ssid", "ESP32-OTA");
  apPasswd   = prefs.getString("ap_pass", "esp32pass");

  // pins
  pinMode(PIN_STM32_BOOT0, OUTPUT);
  pinMode(PIN_STM32_NRST, OUTPUT);
  digitalWrite(PIN_STM32_BOOT0, LOW);
  digitalWrite(PIN_STM32_NRST, HIGH);

  // WiFi mode
  if (wifiMode == "sta") {
    WiFi.onEvent(WiFiStationDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.begin(staSSID.c_str(), staPasswd.c_str());
    Serial.printf("Connecting to STA SSID: %s\n", staSSID.c_str());
  } else {
    // AP (default) or AP+STA (treat as AP for simplicity)
    WiFi.softAP(apSSID.c_str(), apPasswd.c_str());
    Serial.printf("AP started: %s IP: %s\n", apSSID.c_str(), WiFi.softAPIP().toString().c_str());
  }

  // mDNS: use deviceName
  String mdnsName = deviceName;
  mdnsName.replace(" ", "-");
  if (!MDNS.begin(mdnsName.c_str())) {
    Serial.println("Error setting up mDNS responder");
  } else {
    Serial.printf("mDNS responder started: %s.local\n", mdnsName.c_str());
  }

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/setting.html", HTTP_GET, handleSettingsPage);

  // File upload endpoint (multipart form) -> upload handler & done callback
  server.on("/upload_bin", HTTP_POST, handleUploadDone, handleFileUpload);

  // Flash trigger endpoint (POST) – starts background flashing
  server.on("/flash", HTTP_POST, handleFlashTrigger);

  // Flash status endpoint (GET) – polled by web UI
  server.on("/flash_status", HTTP_GET, handleFlashStatus);

  // Settings endpoints
  server.on("/settings", HTTP_GET, handleSettingsGet);
  server.on("/settings", HTTP_POST, handleSettingsPost);

  // Simple health check
  server.on("/ping", HTTP_GET, []() {
    server.send(200, "text/plain", "OK");
  });

  server.begin();
  Serial.println("HTTP server started");
  Serial.println("Ready. Use AT commands over USB serial or upload firmware via web page.");
}

void loop() {
  checkSerialInput();
  server.handleClient();
}
