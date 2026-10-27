
#include "net.h"
#include "config.h"
#include "secrets.h"

#include <WiFi.h>

static uint32_t lastTry = 0;

void netBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // stable latency for UI/touch
}

void netConnectStart() {
  Serial.printf("[WIFI] begin ssid=%s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastTry = millis();
}

void netLoop() {
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) return;

  if (now - lastTry >= WIFI_RETRY_MS) {
    lastTry = now;
    Serial.println("[WIFI] retry...");
    WiFi.disconnect(false);    // keep STA mode
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

bool netIsConnected() {
  return (WiFi.status() == WL_CONNECTED);
}

void netPrintStatus() {
  const wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    Serial.printf("[WIFI] CONNECTED rssi=%d ip=%s\n",
                  WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("[WIFI] status=%d\n", (int)st);
  }
}