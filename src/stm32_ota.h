#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>
#include "stm32_flash.h"

// ---- MQTT OTA topics ----
#define TOPIC_OTA_START   "esp32/ota/start"
#define TOPIC_OTA_CHUNK   "esp32/ota/chunk"
#define TOPIC_OTA_ABORT   "esp32/ota/abort"
#define TOPIC_OTA_FLASH   "esp32/ota/flash"
#define TOPIC_OTA_STATUS  "esp32/ota/status"

// Staging firmware path in LittleFS
#define OTA_FIRMWARE_PATH "/firmware.bin"

// Binary header in each chunk message: 2 bytes for uint16_t index
#define OTA_CHUNK_HEADER  2

enum class OTAState : uint8_t {
    IDLE,
    RECEIVING,
    COMPLETE,
    FLASHING,
    DONE,
    ERROR
};

class STM32OTA {
public:
    STM32OTA(PubSubClient& mqtt, STM32Flasher& flasher)
        : _mqtt(mqtt), _flasher(flasher) {}

    // Call once in setup(): mounts LittleFS.
    bool begin() {
        if (!LittleFS.begin(true)) {
            Serial.println("[OTA] LittleFS mount failed");
            return false;
        }
        Serial.println("[OTA] LittleFS mounted");
        return true;
    }

    // Resubscribe to OTA topics after every MQTT reconnect.
    void subscribeTopics() {
        _mqtt.subscribe(TOPIC_OTA_START);
        _mqtt.subscribe(TOPIC_OTA_CHUNK);
        _mqtt.subscribe(TOPIC_OTA_ABORT);
        _mqtt.subscribe(TOPIC_OTA_FLASH);
    }

    // Dispatch incoming MQTT message by topic.
    void handleMessage(const char* topic, const uint8_t* payload,
                       unsigned int length) {
        if (strcmp(topic, TOPIC_OTA_START) == 0) {
            _onStart(payload, length);
        } else if (strcmp(topic, TOPIC_OTA_CHUNK) == 0) {
            _onChunk(payload, length);
        } else if (strcmp(topic, TOPIC_OTA_ABORT) == 0) {
            _onAbort();
        } else if (strcmp(topic, TOPIC_OTA_FLASH) == 0) {
            _onFlashTrigger();
        }
    }

    // Call every loop(): drives FLASHING state outside the MQTT callback.
    void process() {
        if (!_flashPending || _state != OTAState::COMPLETE) return;
        _flashPending = false;
        _state = OTAState::FLASHING;

        if (!_verifyMD5()) {
            Serial.println("[OTA] MD5 mismatch — aborting flash");
            _publishStatus("error", 0, "MD5 verification failed");
            _state = OTAState::ERROR;
            _resetTransfer();
            return;
        }

        _publishStatus("flashing", 0, "Starting STM32 flash");

        bool ok = _flasher.flashFromLittleFS(OTA_FIRMWARE_PATH,
            [](int pct) {
                Serial.printf("[OTA] Flash progress: %d%%\n", pct);
            });

        if (ok) {
            _state = OTAState::DONE;
            _publishStatus("done", 100, "STM32 flash complete");
        } else {
            _state = OTAState::ERROR;
            _publishStatus("error", 0, "STM32 flash failed");
        }
        _resetTransfer();
    }

    OTAState getState() const { return _state; }

private:
    PubSubClient&  _mqtt;
    STM32Flasher&  _flasher;

    OTAState _state        = OTAState::IDLE;
    uint32_t _totalSize    = 0;
    uint16_t _totalChunks  = 0;
    uint16_t _chunkSize    = 0;
    uint16_t _rxCount      = 0;
    bool     _flashPending = false;
    String   _md5Expected;

    uint8_t* _bitmap       = nullptr;
    uint16_t _bitmapBytes  = 0;

    uint32_t _lastStatusMs = 0;
    static constexpr uint32_t STATUS_INTERVAL_MS = 500;

    // ---- Message handlers ----

    void _onStart(const uint8_t* payload, unsigned int length) {
        if (_state != OTAState::IDLE && _state != OTAState::ERROR
                && _state != OTAState::DONE) {
            Serial.println("[OTA] Transfer already in progress — aborting first");
            _onAbort();
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(
            doc, (const char*)payload, length);
        if (err) {
            Serial.printf("[OTA] Invalid start JSON: %s\n", err.c_str());
            return;
        }

        _totalSize   = doc["size"]       | 0;
        _totalChunks = doc["chunks"]     | 0;
        _chunkSize   = doc["chunk_size"] | 0;
        _md5Expected = doc["md5"].as<String>();

        if (_totalSize == 0 || _totalChunks == 0 || _chunkSize == 0) {
            Serial.println("[OTA] Invalid start parameters");
            return;
        }

        // Allocate chunk tracking bitmap
        _bitmapBytes = (_totalChunks + 7) / 8;
        free(_bitmap);
        _bitmap = (uint8_t*)calloc(_bitmapBytes, 1);
        if (!_bitmap) {
            Serial.println("[OTA] Bitmap alloc failed");
            return;
        }

        _rxCount = 0;

        // Pre-create firmware file at full size (avoids sparse-file seek issues)
        LittleFS.remove(OTA_FIRMWARE_PATH);
        File f = LittleFS.open(OTA_FIRMWARE_PATH, "w");
        if (!f) {
            Serial.println("[OTA] Cannot create firmware file");
            free(_bitmap); _bitmap = nullptr;
            return;
        }
        // Fill with 0xFF (erased flash value) so partial writes are safe
        uint8_t blank[64];
        memset(blank, 0xFF, sizeof(blank));
        for (uint32_t i = 0; i < _totalSize; i += sizeof(blank)) {
            uint32_t toWrite = min((uint32_t)sizeof(blank), _totalSize - i);
            f.write(blank, toWrite);
        }
        f.close();

        _state = OTAState::RECEIVING;
        Serial.printf("[OTA] Ready: %u bytes, %u chunks of %u\n",
                      _totalSize, _totalChunks, _chunkSize);
        _publishStatus("receiving", 0, "Transfer started");
    }

    void _onChunk(const uint8_t* payload, unsigned int length) {
        if (_state != OTAState::RECEIVING) return;
        if (length < OTA_CHUNK_HEADER + 1)  return;

        uint16_t idx = ((uint16_t)payload[0] << 8) | payload[1];
        if (idx >= _totalChunks) return;

        // Skip duplicates
        if (_hasChunk(idx)) return;

        const uint8_t* data    = payload + OTA_CHUNK_HEADER;
        unsigned int   dataLen = length  - OTA_CHUNK_HEADER;

        File f = LittleFS.open(OTA_FIRMWARE_PATH, "r+");
        if (!f) return;
        f.seek((uint32_t)idx * _chunkSize);
        f.write(data, dataLen);
        f.close();

        _markChunk(idx);
        _rxCount++;

        // Throttled progress publish
        if ((millis() - _lastStatusMs) >= STATUS_INTERVAL_MS) {
            int pct = (_rxCount * 100) / _totalChunks;
            char msg[48];
            snprintf(msg, sizeof(msg), "Chunk %u/%u", _rxCount, _totalChunks);
            _publishStatus("receiving", pct, msg);
            _lastStatusMs = millis();
        }

        if (_rxCount == _totalChunks && _isBitmapFull()) {
            _state = OTAState::COMPLETE;
            Serial.println("[OTA] All chunks received");
            _publishStatus("complete", 100,
                           "All chunks received — send esp32/ota/flash to flash");
        }
    }

    void _onAbort() {
        Serial.println("[OTA] Aborted");
        _publishStatus("idle", 0, "Transfer aborted");
        _resetTransfer();
    }

    void _onFlashTrigger() {
        if (_state != OTAState::COMPLETE) {
            _publishStatus("error", 0, "Not ready — transfer incomplete");
            return;
        }
        Serial.println("[OTA] Flash triggered");
        _flashPending = true;
    }

    // ---- Helpers ----

    void _resetTransfer() {
        free(_bitmap);
        _bitmap      = nullptr;
        _bitmapBytes = 0;
        _rxCount     = 0;
        _totalChunks = 0;
        _totalSize   = 0;
        _flashPending = false;
        if (_state != OTAState::DONE) {
            _state = OTAState::IDLE;
        }
    }

    void _markChunk(uint16_t idx) {
        if (_bitmap) _bitmap[idx / 8] |= (1 << (idx % 8));
    }

    bool _hasChunk(uint16_t idx) const {
        if (!_bitmap) return false;
        return (_bitmap[idx / 8] & (1 << (idx % 8))) != 0;
    }

    bool _isBitmapFull() const {
        if (!_bitmap) return false;
        uint16_t fullBytes = _totalChunks / 8;
        for (uint16_t i = 0; i < fullBytes; i++) {
            if (_bitmap[i] != 0xFF) return false;
        }
        uint8_t remainder = _totalChunks % 8;
        if (remainder) {
            uint8_t mask = (1 << remainder) - 1;
            if ((_bitmap[fullBytes] & mask) != mask) return false;
        }
        return true;
    }

    bool _verifyMD5() {
        if (_md5Expected.length() == 0) return true;  // skip if not provided
        MD5Builder md5;
        md5.begin();
        File f = LittleFS.open(OTA_FIRMWARE_PATH, "r");
        if (!f) return false;
        uint8_t buf[256];
        while (f.available()) {
            int n = f.read(buf, sizeof(buf));
            md5.add(buf, n);
        }
        f.close();
        md5.calculate();
        String actual = md5.toString();
        Serial.printf("[OTA] MD5 expected: %s\n", _md5Expected.c_str());
        Serial.printf("[OTA] MD5 actual:   %s\n", actual.c_str());
        return actual == _md5Expected;
    }

    void _publishStatus(const char* state, int progress, const char* message) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\"}",
                 state, progress, message);
        _mqtt.publish(TOPIC_OTA_STATUS, buf);
    }
};
