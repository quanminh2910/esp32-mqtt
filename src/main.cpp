#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "../config/config.h"

#define LED_PIN 2

// Topics
#define TOPIC_PUBLISH   "esp32/test"
#define TOPIC_SUBSCRIBE "esp32/command"

WiFiClient espClient;
PubSubClient mqtt(espClient);

unsigned long lastMqttTest = 0;
int testCounter = 0;

void onMessage(char* topic, byte* payload, unsigned int length) {
  String message;

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.printf("[MQTT] Topic: %s | Message: %s\n", topic, message.c_str());

  if (message == "ON") {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[LED] Turned ON from MQTT");
  } 
  else if (message == "OFF") {
    digitalWrite(LED_PIN, LOW);
    Serial.println("[LED] Turned OFF from MQTT");
  }
}

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(200);
    Serial.print(".");
  }

  digitalWrite(LED_PIN, HIGH);
  Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMessage);

  while (!mqtt.connected()) {
    Serial.printf("[MQTT] Connecting to broker %s:%d ...\n", MQTT_BROKER, MQTT_PORT);

    if (mqtt.connect(MQTT_CLIENT_ID)) {
      Serial.println("[MQTT] Connected");
      mqtt.subscribe(TOPIC_SUBSCRIBE);
      Serial.printf("[MQTT] Subscribed to %s\n", TOPIC_SUBSCRIBE);

      mqtt.publish(TOPIC_PUBLISH, "ESP32 MQTT test connected");
    } else {
      Serial.printf("[MQTT] Failed, rc=%d. Retrying in 2 seconds...\n", mqtt.state());
      delay(2000);
    }
  }
}

void mqttTest() {
  if (millis() - lastMqttTest >= 5000) {
    lastMqttTest = millis();

    String msg = "Hello from ESP32, count = " + String(testCounter++);
    mqtt.publish(TOPIC_PUBLISH, msg.c_str());

    Serial.printf("[MQTT] Published: %s\n", msg.c_str());
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  delay(1000);

  connectWiFi();
  connectMQTT();
}

void loop() {
  // wifi
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost connection — reconnecting...");
    connectWiFi();
  }
  // mqtt
  if (!mqtt.connected()) {
    Serial.println("[MQTT] Lost connection — reconnecting...");
    connectMQTT();
  }

  mqtt.loop();
  mqttTest();

  // Normal blink
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED ON");
  delay(1000);

  digitalWrite(LED_PIN, LOW);
  Serial.println("LED OFF");
  delay(1000);
}