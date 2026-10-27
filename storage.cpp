
#include "storage.h"
#include <LittleFS.h>

static Preferences gPrefs;
static bool gBegun = false;

static void wxInitEmpty(WeatherCache& w, const char* cityName) {
  memset(&w, 0, sizeof(w));
  if (cityName) {
    strlcpy(w.city, cityName, sizeof(w.city));
  } else {
    w.city[0] = 0;
  }
  w.tempC         = NAN;
  w.apparentTempC = NAN;
  w.windKmh       = NAN;
  w.humidity      = -1;
  w.precipMm      = NAN;
  w.uvIdx         = -1;
  w.visibilityKm  = NAN;
  w.wcode         = -1;
  w.updated[0]    = 0;

  for (int i = 0; i < WX_HOURS; i++) { w.hTemp[i] = NAN; w.hCode[i] = -1; }
  for (int d = 0; d < WX_DAYS; d++)  { w.dMin[d]  = NAN; w.dMax[d]  = NAN; }
}

void storageBegin() {
  if (gBegun) return;
  gBegun = true;
  gPrefs.begin("naa", false);

  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    Serial.println("LittleFS: Failed default label 'spiffs'");
    if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
      Serial.println("LittleFS: ERROR - No storage partition found!");
    } else {
      Serial.println("LittleFS: Mounted with label 'littlefs'");
    }
  } else {
    Serial.println("LittleFS: Mounted with label 'spiffs'");
  }

  // Detailed Storage Breakdown for Debugging
  uint32_t total = LittleFS.totalBytes();
  uint32_t used = LittleFS.usedBytes();
  Serial.printf("LITTLEFS: Total: %u bytes, Used: %u bytes, Free: %u bytes\n", 
                total, used, total - used);
  
  File r = LittleFS.open("/", "r");
  if (r) {
    File f = r.openNextFile();
    while(f) {
      Serial.printf("  - %-20s  %u bytes\n", f.name(), (unsigned int)f.size());
      f.close();
      f = r.openNextFile();
    }
    r.close();
  }
}

Preferences& storagePrefs() {
  if (!gBegun) storageBegin();
  return gPrefs;
}

// -------- KV Helpers --------
bool storageGetBool(const char* key, bool defVal) {
  Preferences& p = storagePrefs();
  return p.getBool(key, defVal);
}
void storageSetBool(const char* key, bool v) {
  Preferences& p = storagePrefs();
  p.putBool(key, v);
}

int32_t storageGetI32(const char* key, int32_t defVal) {
  Preferences& p = storagePrefs();
  return p.getInt(key, defVal);
}
void storageSetI32(const char* key, int32_t v) {
  Preferences& p = storagePrefs();
  p.putInt(key, v);
}

void storageRemove(const char* key) {
  Preferences& p = storagePrefs();
  p.remove(key);
}

// -------- Weather Persistence --------
//
// Keys layout:
// wx_cnt is not needed; we store per-index keys.
// For city i:
// wx{i}_city, wx{i}_temp, wx{i}_wind, wx{i}_code, wx{i}_upd

static void makeKey(char* out, size_t cap, int idx, const char* suffix) {
  snprintf(out, cap, "wx%d_%s", idx, suffix);
  out[cap - 1] = 0;
}

void storageLoadWeatherAll(Preferences& prefs, WeatherCache* arr, int n, const char* const* cityNames) {
  if (!arr || n <= 0) return;

  for (int i = 0; i < n; i++) {
    wxInitEmpty(arr[i], cityNames ? cityNames[i] : nullptr);

    char k[24];

    makeKey(k, sizeof(k), i, "city");
    String city = prefs.getString(k, "");
    if (city.length()) strlcpy(arr[i].city, city.c_str(), sizeof(arr[i].city));

    makeKey(k, sizeof(k), i, "temp");
    arr[i].tempC = prefs.getFloat(k, NAN);

    makeKey(k, sizeof(k), i, "wind");
    arr[i].windKmh = prefs.getFloat(k, NAN);

    makeKey(k, sizeof(k), i, "code");
    arr[i].wcode = prefs.getInt(k, -1);

    makeKey(k, sizeof(k), i, "appt");
    arr[i].apparentTempC = prefs.getFloat(k, NAN);

    makeKey(k, sizeof(k), i, "hum");
    arr[i].humidity = prefs.getInt(k, -1);

    makeKey(k, sizeof(k), i, "prec");
    arr[i].precipMm = prefs.getFloat(k, NAN);

    makeKey(k, sizeof(k), i, "uv");
    arr[i].uvIdx = prefs.getInt(k, -1);

    makeKey(k, sizeof(k), i, "vis");
    arr[i].visibilityKm = prefs.getFloat(k, NAN);

    makeKey(k, sizeof(k), i, "upd");
    String upd = prefs.getString(k, "");
    if (upd.length()) strlcpy(arr[i].updated, upd.c_str(), sizeof(arr[i].updated));
  }
}

void storageSaveWeatherOne(Preferences& prefs, int idx, const WeatherCache& wx) {
  if (idx < 0) return;
  char k[24];

  makeKey(k, sizeof(k), idx, "city");
  prefs.putString(k, wx.city);

  makeKey(k, sizeof(k), idx, "temp");
  prefs.putFloat(k, wx.tempC);

  makeKey(k, sizeof(k), idx, "wind");
  prefs.putFloat(k, wx.windKmh);

  makeKey(k, sizeof(k), idx, "code");
  prefs.putInt(k, wx.wcode);

  makeKey(k, sizeof(k), idx, "appt");
  prefs.putFloat(k, wx.apparentTempC);

  makeKey(k, sizeof(k), idx, "hum");
  prefs.putInt(k, wx.humidity);

  makeKey(k, sizeof(k), idx, "prec");
  prefs.putFloat(k, wx.precipMm);

  makeKey(k, sizeof(k), idx, "uv");
  prefs.putInt(k, wx.uvIdx);

  makeKey(k, sizeof(k), idx, "vis");
  prefs.putFloat(k, wx.visibilityKm);

  makeKey(k, sizeof(k), idx, "upd");
  prefs.putString(k, wx.updated);
}

// Convenience wrappers using global prefs
void storageLoadWeatherAll(WeatherCache* arr, int n, const char* const* cityNames) {
  Preferences& p = storagePrefs();
  storageLoadWeatherAll(p, arr, n, cityNames);
}
void storageSaveWeatherOne(int idx, const WeatherCache& wx) {
  Preferences& p = storagePrefs();
  storageSaveWeatherOne(p, idx, wx);
}
