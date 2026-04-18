/**
 * TEST_GLOBALS_S3
 * Prueba objetos globales uno por uno para encontrar cuál crashea.
 * 
 * INSTRUCCIONES:
 * 1. Sube tal como está (solo Preferences + DNSServer + WiFiClient)
 * 2. Si funciona, descomenta el siguiente bloque y sube de nuevo
 * 3. Repite hasta encontrar el que crashea
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>

// ── RONDA 1: Solo primitivos y structs simples ───────────────────────────
Preferences   prefs;
DNSServer     dns;
WiFiClient    wifiClient;

 //── RONDA 2: Descomenta este bloque ─────────────────────────────────────

WebServer     server(80);


// ── RONDA 3: Descomenta este bloque ─────────────────────────────────────

PubSubClient  mqtt(wifiClient);

// ── RONDA 4: Descomenta este bloque ─────────────────────────────────────

WebSocketsServer wsServer(81);

// ── RONDA 5: Descomenta este bloque ─────────────────────────────────────

WebSocketsClient wsClient;

// ── Strings globales ─────────────────────────────────────────────────────
String savedSSID = "";
String deviceSerial = "";
String mqttClientId = "";

void setup() {
  esp_task_wdt_deinit();
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=======================");
  Serial.println("TEST_GLOBALS_S3: OK");
  Serial.println("Todos los globales OK");
  Serial.println("=======================");
}

void loop() {
  static unsigned long t = 0;
  if (millis()-t > 2000) {
    t = millis();
    Serial.println("alive " + String(millis()/1000) + "s");
  }
}
