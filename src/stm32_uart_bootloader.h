#pragma once
#include <Arduino.h>

// STM32 UART Bootloader Commands (AN3155)
#define STM32_CMD_GET          0x00
#define STM32_CMD_GET_VERSION  0x01
#define STM32_CMD_GET_ID       0x02
#define STM32_CMD_READ_MEMORY  0x11
#define STM32_CMD_GO           0x21
#define STM32_CMD_WRITE_MEMORY 0x31
#define STM32_CMD_ERASE        0x43
#define STM32_CMD_EXT_ERASE    0x44
#define STM32_CMD_WRITE_UNPROTECT 0x73

#define STM32_ACK 0x79
#define STM32_NACK 0x1F

class STM32Bootloader {
public:
    STM32Bootloader(HardwareSerial &port, unsigned long defaultTimeoutMs = 1000);

    // Synchronize with the bootloader (send 0x7F)
    bool begin();

    // Read device ID (returns true and sets id)
    bool getID(uint16_t &id);

    // Full/Global erase
    bool eraseFull();

    // Write up to 256 bytes to address (len <= 256)
    bool writeMemory(uint32_t addr, const uint8_t *data, uint16_t len);

    // Read up to 256 bytes from address
    bool readMemory(uint32_t addr, uint8_t *data, uint16_t len);

    // Jump to address (GO)
    bool go(uint32_t addr);

    // Timeout adjustment
    void setTimeout(unsigned long ms) { timeoutMs = ms; }

private:
    HardwareSerial &port;
    unsigned long timeoutMs;

    void flushInput();
    bool waitACK(unsigned long timeout = 0);
    bool sendCommand(uint8_t cmd);
    bool sendAddress(uint32_t address);
    bool sendDataBlock(const uint8_t *data, uint16_t len);
};
