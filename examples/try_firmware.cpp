#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "stm32_flash.h"
#include "stm32_ota.h"

// --- WiFi credentials ---
#define WIFI_SSID      "Mbcare2"
#define WIFI_PASSWORD  "121nguyenthaibinhminh"

// --- MQTT settings ---
#define MQTT_BROKER    "192.168.2.5"
#define MQTT_PORT      1883
#define MQTT_CLIENT_ID "esp32-client"

// --- Topics ---
#define TOPIC_PUBLISH   "esp32/status"
#define TOPIC_SUBSCRIBE "esp32/command"

// --- LED ---
#define LED_PIN 2

// --- Object graph ---
HardwareSerial stm32Serial(2);   // UART2: RX=GPIO16, TX=GPIO17
STM32Flasher   flasher(stm32Serial, STM32_BOOT0_PIN, STM32_NRST_PIN);

WiFiClient     espClient;
PubSubClient   mqtt(espClient);
STM32OTA       ota(mqtt, flasher);

void onMessage(char* topic, byte* payload, unsigned int length) {
  // Route OTA messages to the OTA module
  if (strncmp(topic, "esp32/ota/", 10) == 0) {
    ota.handleMessage(topic, payload, length);
    return;
  }

  // LED control
  if (strcmp(topic, TOPIC_SUBSCRIBE) == 0) {
    String message;
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    Serial.printf("[MQTT] %s → %s\n", topic, message.c_str());
    if (message == "ON")  digitalWrite(LED_PIN, HIGH);
    if (message == "OFF") digitalWrite(LED_PIN, LOW);
  }
}

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.printf("[MQTT] Connecting to %s...\n", MQTT_BROKER);
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      Serial.println("[MQTT] Connected.");
      mqtt.subscribe(TOPIC_SUBSCRIBE);
      ota.subscribeTopics();
      mqtt.publish(TOPIC_PUBLISH, "esp32 online");
    } else {
      Serial.printf("[MQTT] Failed (rc=%d). Retrying in 5s...\n", mqtt.state());
      delay(5000);
    }
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // disable brownout
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  delay(1000);

  connectWiFi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);   // must be set BEFORE connect — allows 229-byte chunks
  mqtt.setKeepAlive(60);     // allow 60s gap during STM32 mass erase

  connectMQTT();
  ota.begin();
}

void loop() {
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();
  ota.process();   // drives FLASHING state outside the MQTT callback
}
