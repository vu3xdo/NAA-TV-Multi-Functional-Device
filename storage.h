
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

// Keep these constants in ONE place only (used by storage.cpp, weather.cpp, ui.cpp)
static constexpr int WX_HOURS = 6;
static constexpr int WX_DAYS  = 5;

// Weather cache structure used everywhere
struct WeatherCache {
  char  city[24];
  float tempC;          // Current temperature (°C)
  float apparentTempC;  // Feels-like temperature (°C)
  float windKmh;        // Wind speed (km/h)
  int   humidity;       // Relative humidity (%)
  float precipMm;       // Precipitation of last hour (mm)
  int   uvIdx;          // UV index (0-11+)
  float visibilityKm;   // Visibility (km)
  int   wcode;          // WMO weather code

  float hTemp[WX_HOURS];
  int   hCode[WX_HOURS];

  float dMin[WX_DAYS];
  float dMax[WX_DAYS];

  char  updated[20];
};

// ---- Core storage lifecycle ----
void storageBegin();
Preferences& storagePrefs();   // returns the global Preferences instance

// ---- Generic KV helpers (used by touch.cpp etc) ----
bool storageGetBool(const char* key, bool defVal);
void storageSetBool(const char* key, bool v);

int32_t storageGetI32(const char* key, int32_t defVal);
void storageSetI32(const char* key, int32_t v);

void storageRemove(const char* key);

// ---- Weather persistence ----
void storageLoadWeatherAll(Preferences& prefs, WeatherCache* arr, int n, const char* const* cityNames);
void storageSaveWeatherOne(Preferences& prefs, int idx, const WeatherCache& wx);

// Convenience wrappers (use global prefs)
void storageLoadWeatherAll(WeatherCache* arr, int n, const char* const* cityNames);
void storageSaveWeatherOne(int idx, const WeatherCache& wx);
