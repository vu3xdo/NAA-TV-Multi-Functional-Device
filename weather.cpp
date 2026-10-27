#include "weather.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include "config.h"

// ---------------- Cities ----------------
const CityDef CITIES[] = {
  { "Rajahmundry",      17.0050f, 81.7780f },
  { "Vishakapatnam",    17.6868f, 83.2185f },
  { "Hyderabad",        17.3850f, 78.4867f },
  { "Delhi",            28.6139f, 77.2090f },
};
const int CITY_COUNT = (int)(sizeof(CITIES) / sizeof(CITIES[0]));

// ---------------- Internals ----------------
static TaskHandle_t sWxTask = nullptr;
static volatile bool sBusy = false;

static volatile bool sDone = false;
static volatile bool sOk   = false;
static volatile int  sCity = 0;

static WeatherCache sResult;
static SemaphoreHandle_t sWxMutex = nullptr;

static RTC_DS3231* sRtc = nullptr;
static bool sRtcOk = false;

static bool haveSystemTime() {
  time_t now = time(nullptr);
  return (now > (time_t)1700000000); // ~2023-11
}

// RTC -> system/NTP -> NO_TIME
static void stampNow(char* out20, RTC_DS3231& rtc, bool rtcOK) {
  if (!out20) return;

  if (rtcOK) {
    // PRO UPGRADE: Fetch True UTC from RTC, apply offset directly
    DateTime now(rtc.now().unixtime() + IST_OFFSET_SEC);
    snprintf(out20, 20, "%04d-%02d-%02d %02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute());
    out20[19] = 0;
    return;
  }

  if (WiFi.status() == WL_CONNECTED && haveSystemTime()) {
    time_t t = time(nullptr);
    struct tm tmNow;
    localtime_r(&t, &tmNow);
    snprintf(out20, 20, "%04d-%02d-%02d %02d:%02d",
             tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
             tmNow.tm_hour, tmNow.tm_min);
    out20[19] = 0;
    return;
  }

  strncpy(out20, "NO_TIME", 20);
  out20[19] = 0;
}

const char* wxCodeLabel(int code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mainly Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Overcast";
    case 45: return "Fog";
    case 48: return "Rime Fog";
    case 51: return "Drizzle (L)";
    case 53: return "Drizzle (M)";
    case 55: return "Drizzle (H)";
    case 56: return "Freezing Drz (L)";
    case 57: return "Freezing Drz (H)";
    case 61: return "Rain (L)";
    case 63: return "Rain (M)";
    case 65: return "Rain (H)";
    case 66: return "Freezing Rain (L)";
    case 67: return "Freezing Rain (H)";
    case 71: return "Snow (L)";
    case 73: return "Snow (M)";
    case 75: return "Snow (H)";
    case 77: return "Snow Grains";
    case 80: return "Showers (L)";
    case 81: return "Showers (M)";
    case 82: return "Showers (V)";
    case 85: return "Snow Showers (L)";
    case 86: return "Snow Showers (H)";
    case 95: return "Thunderstorm";
    case 96: return "T-Storm + Hail (L)";
    case 99: return "T-Storm + Hail (H)";
    default: return "Unknown";
  }
}

static bool httpGetJson(const String& url, DynamicJsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);

  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code <= 0) { http.end(); return false; }

  String body = http.getString();
  http.end();

  if (body.length() < 20) return false;

  DeserializationError err = deserializeJson(doc, body);
  return !err;
}

bool weatherFetchCity(int cityIdx, WeatherCache& outWx, RTC_DS3231& rtc, bool rtcOK) {
  if (cityIdx < 0 || cityIdx >= CITY_COUNT) return false;

  memset(&outWx, 0, sizeof(outWx));
  outWx.tempC         = NAN;
  outWx.apparentTempC = NAN;
  outWx.windKmh       = NAN;
  outWx.humidity      = -1;
  outWx.precipMm      = NAN;
  outWx.uvIdx         = -1;
  outWx.visibilityKm  = NAN;
  outWx.wcode         = -1;
  outWx.updated[0]    = 0;

  strlcpy(outWx.city, CITIES[cityIdx].name, sizeof(outWx.city));

  for (int i = 0; i < WX_HOURS; i++) { outWx.hTemp[i] = NAN; outWx.hCode[i] = -1; }
  for (int d = 0; d < WX_DAYS;  d++) { outWx.dMin[d]  = NAN; outWx.dMax[d]  = NAN; }

  // Single request — all 7 current fields fetched atomically.
  // Open-Meteo free tier, no API key required.
  String url =
    String("https://api.open-meteo.com/v1/forecast?latitude=") + String(CITIES[cityIdx].lat, 4) +
    "&longitude=" + String(CITIES[cityIdx].lon, 4) +
    "&current=temperature_2m,apparent_temperature,wind_speed_10m,"
             "relative_humidity_2m,precipitation,uv_index,visibility,weather_code"
    "&wind_speed_unit=kmh"
    "&timezone=Asia%2FKolkata";

  DynamicJsonDocument doc(16 * 1024);
  if (!httpGetJson(url, doc)) {
    stampNow(outWx.updated, rtc, rtcOK);
    return false;
  }

  JsonObject cur = doc["current"];
  if (cur.isNull()) {
    stampNow(outWx.updated, rtc, rtcOK);
    return false;
  }

  outWx.tempC         = cur["temperature_2m"]         | NAN;
  outWx.apparentTempC = cur["apparent_temperature"]   | NAN;
  outWx.windKmh       = cur["wind_speed_10m"]         | NAN;
  outWx.humidity      = cur["relative_humidity_2m"]   | -1;
  outWx.precipMm      = cur["precipitation"]          | NAN;
  outWx.uvIdx         = cur["uv_index"]               | -1;
  // visibility comes in metres — convert to km
  float visM = cur["visibility"] | NAN;
  outWx.visibilityKm  = isnan(visM) ? NAN : (visM / 1000.0f);
  outWx.wcode         = cur["weather_code"]           | -1;

  stampNow(outWx.updated, rtc, rtcOK);
  return true;
}

// ---------------- Async task ----------------
static void wxTask(void* arg) {
  int city = (int)(intptr_t)arg;

  WeatherCache tmp;
  bool ok = false;

  if (sRtc) ok = weatherFetchCity(city, tmp, *sRtc, sRtcOk);

  if (sWxMutex && xSemaphoreTake(sWxMutex, portMAX_DELAY)) {
    if (ok) {
      sResult = tmp;
    }
    sOk = ok;
    sCity = city;
    sDone = true;
    xSemaphoreGive(sWxMutex);
  }
  sBusy = false;

  sWxTask = nullptr;
  vTaskDelete(nullptr);
}

void weatherBegin() {
  sWxMutex = xSemaphoreCreateMutex();
}

bool weatherIsBusy() { return sBusy; }

void weatherRequestFetch(int cityIdx) {
  if (cityIdx < 0 || cityIdx >= CITY_COUNT) return;
  if (sBusy) return;

  sBusy = true;
  sDone = false;
  sOk   = false;
  sCity = cityIdx;

  xTaskCreatePinnedToCore(
    wxTask,
    "wxTask",
    8192,
    (void*)(intptr_t)cityIdx,
    1,
    &sWxTask,
    0
  );
}

bool weatherConsumeResult(bool& ok, WeatherCache& wx, int& cityIdx) {
  if (!sDone) return false;

  if (sWxMutex && xSemaphoreTake(sWxMutex, portMAX_DELAY)) {
    ok = sOk;
    cityIdx = sCity;
    wx = sResult;
    sDone = false;
    xSemaphoreGive(sWxMutex);
  }

  return true;
}

void weatherLoop() {}

// legacy helper; no ui color dependencies
void weatherDrawScreen(Adafruit_ILI9341& tft, int citySel, const WeatherCache& wx) {
  tft.fillRect(0, 20, 320, 240 - 20 - 18, ILI9341_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(ILI9341_CYAN, ILI9341_BLACK);
  tft.setCursor(10, 30);
  tft.print("City: ");
  if (citySel >= 0 && citySel < CITY_COUNT) tft.print(CITIES[citySel].name);

  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.setCursor(10, 75);
  tft.print("Temp: ");
  if (isnan(wx.tempC)) tft.print("--");
  else { tft.print(wx.tempC, 1); tft.print(" C"); }

  tft.setCursor(10, 105);
  tft.print("Wind: ");
  if (isnan(wx.windKmh)) tft.print("--");
  else { tft.print(wx.windKmh, 1); tft.print(" km/h"); }

  tft.setCursor(10, 135);
  tft.print("Now: ");
  tft.print(wxCodeLabel(wx.wcode));

  tft.setTextSize(1);
  tft.setTextColor(ILI9341_LIGHTGREY, ILI9341_BLACK);
  tft.setCursor(10, 170);
  tft.print("Last: ");
  tft.print(wx.updated[0] ? wx.updated : "None");
}

void weatherAttachRTC(RTC_DS3231* rtc, bool ok) {
  sRtc = rtc;
  sRtcOk = ok;
}