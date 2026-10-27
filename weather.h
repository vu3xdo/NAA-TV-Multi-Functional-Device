
#pragma once
#include <Arduino.h>
#include <RTClib.h>
#include <Adafruit_ILI9341.h>

#include "storage.h"   // WeatherCache + WX_HOURS/WX_DAYS live here

// City database
struct CityDef {
  const char* name;
  float lat;
  float lon;
};

// Exported city table + count
extern const CityDef CITIES[];
extern const int CITY_COUNT;

// Human-readable weather label from Open-Meteo weather_code
const char* wxCodeLabel(int code);

// Fetch (sync) for one city index (fills cache, stamps updated time)
bool weatherFetchCity(int cityIdx, WeatherCache& outWx, RTC_DS3231& rtc, bool rtcOK);

// ===== Async (non-blocking) interface used by your main loop/UI =====
void weatherBegin();
bool weatherIsBusy();
void weatherRequestFetch(int cityIdx);

// When a fetch finishes, you can consume the result once.
// returns true if there was a finished result ready to consume.
bool weatherConsumeResult(bool& ok, WeatherCache& wx, int& cityIdx);

// Convenience: call this from loop() every frame
void weatherLoop();

// Optional: legacy draw helper (if you still call it somewhere)
void weatherDrawScreen(Adafruit_ILI9341& tft, int citySel, const WeatherCache& wx);

// Optional: allow attaching RTC pointer from main/sensors
extern "C" void weatherAttachRTC(RTC_DS3231* rtc, bool ok);
