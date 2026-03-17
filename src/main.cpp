// Simple LED blink for ESP-WROOM-32 (esp32dev)
// GPIO 2 = built-in LED (or connect external LED + 220Ω resistor to any GPIO)
//
// To use: copy this content into src/main.cpp, replacing the current code.

#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

#define LED_PIN 2

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Fast blink while connecting
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(200);
    Serial.print(".");
  }

  digitalWrite(LED_PIN, HIGH);  // solid ON = connected
  Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  delay(1000);

  connectWiFi();
}

void loop() {
  // Re-test connection every loop
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost connection — reconnecting...");
    connectWiFi();
  }

  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED ON");
  delay(1000);
  digitalWrite(LED_PIN, LOW);
  Serial.println("LED OFF");
  delay(1000);
}

