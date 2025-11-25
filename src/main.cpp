/*
  esp32_stm32_ota.ino  (fixed upload + bootloader sync)
  - Webserver with correct multipart upload handling
  - STM32 UART bootloading uses SERIAL_8E1 and correct timing
  - AT commands saved to Preferences
*/

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include "stm32_uart_bootloader.h"

// -------- CONFIG: change pins to match your board --------
#define PIN_UART_TX 6   // ESP32 D6 -> STM32 RX D0
#define PIN_UART_RX 7   // ESP32 D7 <- STM32 TX D1

#define PIN_STM32_BOOT0 4
#define PIN_STM32_NRST  5

#define STM32_BAUD 115200
const char *FIRM_FILE = "/firmware.bin";

WebServer server(80);
Preferences prefs;
const char *PREF_NS = "esp_cfg";

HardwareSerial SerialSTM(1);
STM32Bootloader stmboot(SerialSTM, 1000);

// Helper for hardware control
void stm_set_boot0(bool high) { digitalWrite(PIN_STM32_BOOT0, high ? HIGH : LOW); }
void stm_reset_pulse() { digitalWrite(PIN_STM32_NRST, LOW); delay(50); digitalWrite(PIN_STM32_NRST, HIGH); delay(50); }

void enter_bootloader_hw() {
  pinMode(PIN_STM32_BOOT0, OUTPUT);
  pinMode(PIN_STM32_NRST, OUTPUT);
  stm_set_boot0(true);
  delay(10);
  // pulse reset low->high
  stm_reset_pulse();
  delay(100);
}

void exit_bootloader_hw() {
  stm_set_boot0(false);
  delay(10);
  stm_reset_pulse();
  delay(50);
}

// ---------- Web upload handling using HTTPUpload ----------
void handleRoot() {
  if (SPIFFS.exists("/ota_index.html")) {
    File f = SPIFFS.open("/ota_index.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  } else {
    server.send(200, "text/plain", "Upload page not found. Put ota_index.html into SPIFFS.");
  }
}

void handleUploadDone() {
  server.send(200, "text/plain", "OK");
}

void handleFileUpload() {
  HTTPUpload &upload = server.upload();
  static File uploadFile;
  if (upload.status == UPLOAD_FILE_START) {
    if (SPIFFS.exists(FIRM_FILE)) SPIFFS.remove(FIRM_FILE);
    uploadFile = SPIFFS.open(FIRM_FILE, FILE_WRITE);
    if (!uploadFile) {
      Serial.println("Failed to open file for upload");
    } else {
      Serial.printf("Upload start: %s\n", upload.filename.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload finished, %u bytes\n", (unsigned)upload.totalSize);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
      SPIFFS.remove(FIRM_FILE);
      Serial.println("Upload aborted");
    }
  }
}

// ---------- Flash trigger endpoint ----------
void handleFlashTrigger() {
  if (!SPIFFS.exists(FIRM_FILE)) {
    server.send(400, "text/plain", "No firmware found in SPIFFS");
    return;
  }
  server.send(200, "text/plain", "Flashing started");

  // Blocking flash sequence
  File f = SPIFFS.open(FIRM_FILE, FILE_READ);
  if (!f) {
    Serial.println("Failed to open firmware file");
    return;
  }

  // Enter bootloader via HW
  enter_bootloader_hw();

  // Start UART with EVEN parity (8E1) required by many STM32 bootloaders
  SerialSTM.begin(STM32_BAUD, SERIAL_8E1, PIN_UART_RX, PIN_UART_TX);
  delay(50);
  stmboot.setTimeout(1500); // increase timeouts for reliability
  bool ok = false;

  // Try sync multiple times (some boards take a short time)
  const int tries = 3;
  int i;
  for (i = 0; i < tries; ++i) {
    if (stmboot.begin()) break;
    Serial.printf("Sync attempt %d failed, retrying...\n", i+1);
    delay(200);
  }

  // Stream file in 256-byte chunks
  const size_t CHUNK = 256;
  uint8_t buf[CHUNK];
  uint32_t addr = 0x08000000UL;
  size_t remaining = f.size();
  
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

  while (remaining > 0) {
    size_t toRead = remaining > CHUNK ? CHUNK : remaining;
    size_t r = f.read(buf, toRead);
    if (r == 0) {
      Serial.println("File read error");
      goto finish;
    }
    if (!stmboot.writeMemory(addr, buf, (uint16_t)r)) {
      Serial.printf("Write failed at 0x%08X\n", addr);
      goto finish;
    }
    addr += r;
    remaining -= r;
    Serial.printf("Wrote %u bytes to 0x%08X\n", (unsigned)r, (unsigned)(addr - r));
    delay(5);
  }

  // GO to app
  if (!stmboot.go(0x08000000UL)) {
    Serial.println("GO failed");
    goto finish;
  }
  Serial.println("GO OK");
  ok = true;

finish:
  f.close();
  SerialSTM.end();
  // leave bootloader and run application
  exit_bootloader_hw();
  Serial.printf("Flashing finished: %s\n", ok ? "OK" : "FAIL");
}

// ---------- AT command handling ----------
String serialLine = "";
void handleATCommand(const String &cmd) {
  String s = cmd; s.trim();
  if (s.length() == 0) return;

  Serial.printf("AT: %s\n", s.c_str());
  if (s.startsWith("AT+WIFI_MODE")) {
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
    Serial.printf("wifi_mode=%s\n", prefs.getString("wifi_mode", "WIFI_AP").c_str());
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

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32 STM32 OTA Bridge (fixed)");

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
    while (1) delay(1000);
  }
  prefs.begin(PREF_NS, false);

  pinMode(PIN_STM32_BOOT0, OUTPUT);
  pinMode(PIN_STM32_NRST, OUTPUT);
  digitalWrite(PIN_STM32_BOOT0, LOW);
  digitalWrite(PIN_STM32_NRST, HIGH);

  // Start WiFi (AP by default)
  String mode = prefs.getString("wifi_mode", "WIFI_AP");
  if (mode == "WIFI_STA") {
    String ss = prefs.getString("sta_ssid", "");
    String pw = prefs.getString("sta_pass", "");
    WiFi.begin(ss.c_str(), pw.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
    Serial.printf("WiFi status: %d\n", WiFi.status());
  } else {
    String ap_ssid = prefs.getString("ap_ssid", "ESP32-OTA");
    String ap_pass = prefs.getString("ap_pass", "esp32pass");
    WiFi.softAP(ap_ssid.c_str(), ap_pass.c_str());
    Serial.printf("AP started: %s IP: %s\n", ap_ssid.c_str(), WiFi.softAPIP().toString().c_str());
  }

  // Web endpoints
  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload_bin", HTTP_POST, handleUploadDone, handleFileUpload);
  server.on("/flash", HTTP_GET, handleFlashTrigger);
  server.begin();

  Serial.println("Ready. Use AT commands over USB serial or upload firmware via web page.");
}

void loop() {
  checkSerialInput();
  server.handleClient();
}
