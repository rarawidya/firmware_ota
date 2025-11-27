#include "stm32_uart_bootloader.h"

STM32Bootloader::STM32Bootloader(HardwareSerial &port, unsigned long defaultTimeoutMs)
  : port(port), timeoutMs(defaultTimeoutMs) {}

void STM32Bootloader::flushInput() {
  while (port.available()) port.read();
}

bool STM32Bootloader::begin() {
  flushInput();
  uint8_t sync = 0x7F;
  port.write(&sync, 1);
  port.flush();
  return waitACK(timeoutMs);
}

bool STM32Bootloader::waitACK(unsigned long timeout) {
  if (timeout == 0) timeout = timeoutMs;
  unsigned long start = millis();
  while (millis() - start < timeout) {
    if (port.available()) {
      int r = port.read();
      if (r == STM32_ACK) return true;
      if (r == STM32_NACK) return false;
    }
    delay(1);
  }
  return false;
}

bool STM32Bootloader::sendCommand(uint8_t cmd) {
  uint8_t buf[2] = { cmd, (uint8_t)(cmd ^ 0xFF) };
  port.write(buf, 2);
  port.flush();
  return waitACK(timeoutMs);
}

bool STM32Bootloader::sendAddress(uint32_t address) {
  uint8_t addr[5];
  addr[0] = (address >> 24) & 0xFF;
  addr[1] = (address >> 16) & 0xFF;
  addr[2] = (address >> 8) & 0xFF;
  addr[3] = (address >> 0) & 0xFF;
  addr[4] = addr[0] ^ addr[1] ^ addr[2] ^ addr[3];
  port.write(addr, 5);
  port.flush();
  return waitACK(timeoutMs);
}

bool STM32Bootloader::sendDataBlock(const uint8_t *data, uint16_t len) {
  if (len == 0 || len > 256) return false;
  uint8_t count = (uint8_t)(len - 1);
  uint8_t ck = count;
  port.write(&count, 1);
  for (uint16_t i = 0; i < len; ++i) { port.write(data[i]); ck ^= data[i]; }
  port.write(ck);
  port.flush();
  return waitACK(timeoutMs * 2);
}

bool STM32Bootloader::getID(uint16_t &id) {
  if (!sendCommand(STM32_CMD_GET_ID)) return false;

  unsigned long start = millis();
  while (!port.available() && (millis() - start < 300)) delay(1);
  if (!port.available()) return false;

  int len = port.read(); 
  if (len < 0) return false;
  int readCount = len + 1;
  uint8_t buf[4] = {0};
  for (int i = 0; i < readCount; ++i) {
    unsigned long s = millis();
    while (!port.available() && (millis() - s < 300)) delay(1);
    if (!port.available()) return false;
    buf[i] = (uint8_t)port.read();
  }
  if (!waitACK(timeoutMs)) return false;

  id = ((uint16_t)buf[0] << 8) | buf[1];
  return true;
}

bool STM32Bootloader::eraseFull() {
  if (!sendCommand(STM32_CMD_EXT_ERASE)) return false;
  uint8_t seq[3] = { 0xFF, 0xFF, (uint8_t)(0xFF ^ 0xFF) };
  port.write(seq, 3);
  port.flush();
  return waitACK(8000); 
}

bool STM32Bootloader::writeMemory(uint32_t addr, const uint8_t *data, uint16_t len) {
  if (len == 0 || len > 256) return false;
  if (!sendCommand(STM32_CMD_WRITE_MEMORY)) return false;
  if (!sendAddress(addr)) return false;
  if (!sendDataBlock(data, len)) return false;
  return true;
}

bool STM32Bootloader::readMemory(uint32_t addr, uint8_t *data, uint16_t len) {
  if (len == 0 || len > 256) return false;
  if (!sendCommand(STM32_CMD_READ_MEMORY)) return false;
  if (!sendAddress(addr)) return false;

  uint8_t n = (uint8_t)(len - 1);
  uint8_t ck = n ^ 0xFF;
  port.write(n);
  port.write(ck);
  port.flush();

  if (!waitACK(timeoutMs)) return false;

  unsigned long start = millis();
  uint16_t got = 0;
  while (got < len && (millis() - start < timeoutMs)) {
    if (port.available()) {
      data[got++] = (uint8_t)port.read();
    } else {
      delay(1);
    }
  }
  return got == len;
}

bool STM32Bootloader::go(uint32_t addr) {
  if (!sendCommand(STM32_CMD_GO)) return false;
  if (!sendAddress(addr)) return false;
  return true;
}
