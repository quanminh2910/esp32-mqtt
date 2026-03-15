#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

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

WiFiClient espClient;
PubSubClient mqtt(espClient);

void onMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.printf("[MQTT] Topic: %s | Message: %s\n", topic, message.c_str());

  // Control LED via MQTT command
  if (String(topic) == TOPIC_SUBSCRIBE) {
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
      mqtt.publish(TOPIC_PUBLISH, "esp32 online");
    } else {
      Serial.printf("[MQTT] Failed (rc=%d). Retrying in 5s...\n", mqtt.state());
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  connectWiFi();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMessage);

  connectMQTT();
}

void loop() {
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();
}
