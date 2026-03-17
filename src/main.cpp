// Simple LED blink for ESP-WROOM-32 (esp32dev)
// GPIO 2 = built-in LED (or connect external LED + 220Ω resistor to any GPIO)
//
// To use: copy this content into src/main.cpp, replacing the current code.

#include <Arduino.h>

#define LED_PIN 2  // GPIO 2 = built-in LED on ESP-WROOM-32

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_PIN, HIGH);  // LED on
  delay(500);
  digitalWrite(LED_PIN, LOW);   // LED off
  delay(500);
}
