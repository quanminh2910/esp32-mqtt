#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>
#include <LittleFS.h>

// ---- Hardware pin configuration ----
#define STM32_UART_TX    17
#define STM32_UART_RX    16
#define STM32_BOOT0_PIN   4
#define STM32_NRST_PIN    5
#define STM32_UART_BAUD   57600

// ---- AN3155 constants ----
#define STM32_ACK        0x79
#define STM32_NACK       0x1F
#define STM32_SYNC_BYTE  0x7F

#define STM32_CMD_GO         0x21
#define STM32_CMD_WRITE_MEM  0x31
#define STM32_CMD_EXT_ERASE  0x44

#define STM32_FLASH_BASE  0x08000000UL
#define STM32_WRITE_BLOCK 256

#define STM32_ACK_TIMEOUT_MS    2000
#define STM32_ERASE_TIMEOUT_MS  30000
#define STM32_WRITE_TIMEOUT_MS  2000

class STM32Flasher {
public:
    STM32Flasher(HardwareSerial& serial, int boot0Pin, int nrstPin)
        : _serial(serial), _boot0Pin(boot0Pin), _nrstPin(nrstPin) {}

    // Flash a binary stored in LittleFS. progressCb(0-100) is optional.
    bool flashFromLittleFS(const char* filepath,
                           void (*progressCb)(int) = nullptr) {
        File f = LittleFS.open(filepath, "r");
        if (!f) {
            Serial.println("[STM32] Cannot open firmware file");
            return false;
        }
        size_t fileSize = f.size();
        if (fileSize == 0) { f.close(); return false; }

        if (!begin()) {
            Serial.println("[STM32] Sync failed");
            f.close();
            return false;
        }

        if (!eraseAll()) {
            Serial.println("[STM32] Erase failed");
            f.close();
            end();
            return false;
        }

        uint8_t  buf[STM32_WRITE_BLOCK];
        uint32_t addr     = STM32_FLASH_BASE;
        size_t   written  = 0;

        while (f.available()) {
            int n = f.read(buf, STM32_WRITE_BLOCK);
            if (n <= 0) break;

            // Pad last block to 4-byte boundary
            while (n % 4 != 0) buf[n++] = 0xFF;

            if (!writeBlock(addr, buf, (uint8_t)n)) {
                Serial.printf("[STM32] Write failed at 0x%08X\n", addr);
                f.close();
                end();
                return false;
            }
            addr    += n;
            written += n;

            if (progressCb) {
                progressCb((int)((written * 100UL) / fileSize));
            }
        }

        f.close();
        go(STM32_FLASH_BASE);
        end();
        Serial.println("[STM32] Flash complete");
        return true;
    }

    // Sync with STM32 ROM bootloader. Returns true when ready.
    bool begin() {
        _enterBootloader();
        for (int attempt = 0; attempt < 3; attempt++) {
            while (_serial.available()) _serial.read();  // flush
            _serial.write(STM32_SYNC_BYTE);
            if (_waitACK(2000)) {
                Serial.println("[STM32] Sync OK");
                return true;
            }
            Serial.printf("[STM32] Sync attempt %d failed\n", attempt + 1);
            _enterBootloader();  // re-enter and retry
        }
        return false;
    }

    // Mass erase all user flash via Extended Erase (AN3155 cmd 0x44).
    bool eraseAll() {
        if (!_sendCommand(STM32_CMD_EXT_ERASE)) return false;
        // Special "global mass erase" code: 0xFFFF, checksum 0x00
        uint8_t massErase[3] = { 0xFF, 0xFF, 0x00 };
        _serial.write(massErase, 3);
        Serial.println("[STM32] Erasing... (up to 30s)");
        return _waitACK(STM32_ERASE_TIMEOUT_MS);
    }

    // Write up to 256 bytes to flash at 'address' (must be 4-byte aligned).
    bool writeBlock(uint32_t address, const uint8_t* data, uint8_t len) {
        if (!_sendCommand(STM32_CMD_WRITE_MEM)) return false;
        if (!_sendAddress(address))             return false;
        if (!_sendDataBlock(data, len))         return false;
        return true;
    }

    // Jump to application at 'address'.
    bool go(uint32_t address = STM32_FLASH_BASE) {
        if (!_sendCommand(STM32_CMD_GO)) return false;
        _sendAddress(address);  // STM32 jumps — no ACK expected
        return true;
    }

    // Release bootloader mode, reset STM32 into normal boot.
    void end() {
        _exitBootloader();
    }

private:
    HardwareSerial& _serial;
    int _boot0Pin;
    int _nrstPin;

    // BOOT0 HIGH + NRST pulse → STM32 ROM bootloader starts
    void _enterBootloader() {
        _serial.end();
        pinMode(_boot0Pin, OUTPUT);
        digitalWrite(_boot0Pin, HIGH);
        pinMode(_nrstPin, OUTPUT);
        digitalWrite(_nrstPin, LOW);
        delay(10);
        pinMode(_nrstPin, INPUT);   // release NRST (open-drain safe)
        delay(100);
        _serial.begin(STM32_UART_BAUD, SERIAL_8E1,
                      STM32_UART_RX, STM32_UART_TX);
    }

    // BOOT0 LOW + NRST pulse → STM32 boots normally
    void _exitBootloader() {
        _serial.end();
        digitalWrite(_boot0Pin, LOW);
        pinMode(_nrstPin, OUTPUT);
        digitalWrite(_nrstPin, LOW);
        delay(10);
        pinMode(_nrstPin, INPUT);
        delay(100);
    }

    // Send [cmd, ~cmd], wait for ACK
    bool _sendCommand(uint8_t cmd) {
        uint8_t frame[2] = { cmd, (uint8_t)(~cmd) };
        _serial.write(frame, 2);
        return _waitACK(STM32_ACK_TIMEOUT_MS);
    }

    // Send 4-byte big-endian address + XOR checksum, wait for ACK
    bool _sendAddress(uint32_t addr) {
        uint8_t buf[5];
        buf[0] = (addr >> 24) & 0xFF;
        buf[1] = (addr >> 16) & 0xFF;
        buf[2] = (addr >>  8) & 0xFF;
        buf[3] = (addr      ) & 0xFF;
        buf[4] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
        _serial.write(buf, 5);
        return _waitACK(STM32_ACK_TIMEOUT_MS);
    }

    // Send [len-1, data[0]...data[len-1], XOR(len-1 ^ all data)], wait ACK
    bool _sendDataBlock(const uint8_t* data, uint8_t len) {
        uint8_t checksum = (uint8_t)(len - 1);
        _serial.write(checksum);
        for (uint8_t i = 0; i < len; i++) {
            _serial.write(data[i]);
            checksum ^= data[i];
        }
        _serial.write(checksum);
        return _waitACK(STM32_WRITE_TIMEOUT_MS);
    }

    // Block until ACK (0x79) or NACK (0x1F), or timeout
    bool _waitACK(uint32_t timeoutMs) {
        uint32_t deadline = millis() + timeoutMs;
        while (millis() < deadline) {
            int b = _readByte(50);
            if (b == STM32_ACK)  return true;
            if (b == STM32_NACK) return false;
        }
        return false;
    }

    // Read one byte with timeout; returns -1 on timeout
    int _readByte(uint32_t timeoutMs) {
        uint32_t deadline = millis() + timeoutMs;
        while (millis() < deadline) {
            if (_serial.available()) return _serial.read();
        }
        return -1;
    }
};
