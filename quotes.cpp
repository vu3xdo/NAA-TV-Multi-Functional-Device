
#include "quotes.h"
#include "config.h"
#include "secrets.h"
#include "net.h"
#include "sensors.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

static Preferences pref;

static QuoteData cached;
static volatile bool busy = false;
static volatile bool haveResult = false;
static volatile bool resultOk = false;
static QuoteData resultData;

static TaskHandle_t taskH = nullptr;
// PRO UPGRADE: Thread Safety Mutex
static SemaphoreHandle_t dataMutex = nullptr;

static void safeStrcpy(char* dst, size_t cap, const String& s) {
  if (!dst || cap == 0) return;
  String t = s;
  t.replace("\r", " ");
  t.replace("\n", " ");
  t.trim();
  while (t.indexOf("  ") >= 0) t.replace("  ", " ");
  strncpy(dst, t.c_str(), cap);
  dst[cap - 1] = 0;
}

static void stampNow(char out[20]) {
  if (!sensorsRtcOk()) { strncpy(out, "NO_RTC", 20); out[19] = 0; return; }
  DateTime dt = sensorsNow();
  snprintf(out, 20, "%04d-%02d-%02d %02d:%02d",
           dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute());
  out[19] = 0;
}

static void loadCache() {
  memset(&cached, 0, sizeof(cached));
  pref.begin("naa", true);
  String q = pref.getString("q_txt", "");
  String a = pref.getString("q_auth", "");
  String u = pref.getString("q_upd", "");
  pref.end();

  safeStrcpy(cached.quote, sizeof(cached.quote), q);
  safeStrcpy(cached.author, sizeof(cached.author), a);
  safeStrcpy(cached.updated, sizeof(cached.updated), u);

  if (cached.quote[0] == 0) safeStrcpy(cached.quote, sizeof(cached.quote), String("No quote cached yet."));
  if (cached.updated[0] == 0) safeStrcpy(cached.updated, sizeof(cached.updated), String("None"));
}

static void saveCache(const QuoteData& qd) {
  pref.begin("naa", false);
  pref.putString("q_txt", qd.quote);
  pref.putString("q_auth", qd.author);
  pref.putString("q_upd", qd.updated);
  pref.end();
}

static bool fetchZenOnce(QuoteData& out) {
  if (WiFi.status() != WL_CONNECTED || !netIsConnected()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8);

  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url = String("https://zenquotes.io/api/random?_=") + String(millis());
  if (!http.begin(client, url)) return false;
  http.addHeader("Accept", "application/json");
  http.addHeader("Cache-Control", "no-cache");
  http.addHeader("User-Agent", "NAA_TV/1.0");

  int rc = http.GET();
  String body = http.getString();
  http.end();

  if (rc != 200 || body.length() < 8) return false;

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, body)) return false;

  JsonObject o = doc[0];
  String q = String((const char*)(o["q"] | ""));
  String a = String((const char*)(o["a"] | ""));

  q.trim(); a.trim();
  if (q.length() == 0) return false;

  safeStrcpy(out.quote, sizeof(out.quote), q);
  safeStrcpy(out.author, sizeof(out.author), a);
  stampNow(out.updated);
  return true;
}

static void quotesTask(void*) {
  QuoteData out;
  memset(&out, 0, sizeof(out));

  bool ok = fetchZenOnce(out);
  if (!ok) {
    uint32_t t0 = millis();
    while (millis() - t0 < 500) { vTaskDelay(10 / portTICK_PERIOD_MS); }
    ok = fetchZenOnce(out);
  }
  if (!ok) out = cached;

  if (dataMutex && xSemaphoreTake(dataMutex, portMAX_DELAY)) {
    resultData = out;
    resultOk = ok;
    haveResult = true;
    xSemaphoreGive(dataMutex);
  }
  busy = false;

  taskH = nullptr;
  vTaskDelete(nullptr);
}

void quotesBegin() {
  dataMutex = xSemaphoreCreateMutex();
  loadCache();
}
void quotesLoop() {}

bool quotesIsBusy() { return busy; }

void quotesRequestFetch() {
  if (busy) return;
  busy = true;
  haveResult = false;
  resultOk = false;

  xTaskCreatePinnedToCore(quotesTask, "quotesTask", 8192, nullptr, 1, &taskH, 0);
}

bool quotesConsumeResult(bool& ok, QuoteData& out) {
  if (!haveResult) return false;
  
  if (dataMutex && xSemaphoreTake(dataMutex, portMAX_DELAY)) {
    haveResult = false;
    ok = resultOk;
    out = resultData;

    if (out.quote[0]) {
      cached = out;
      saveCache(cached);
    }
    xSemaphoreGive(dataMutex);
  }
  return true;
}

const QuoteData& quotesGetCached() { return cached; }

void quotesNext() {
  // Only cycle through cached if we had multiple, but QuoteData only has one.
  // We remove auto-fetch on tap as requested.
}
