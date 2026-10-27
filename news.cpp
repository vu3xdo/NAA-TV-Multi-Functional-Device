
#include "news.h"
#include "config.h"
#include "secrets.h"
#include "net.h"
#include "sensors.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <LittleFS.h>

static Preferences pref;

static NewsData cached;
static int curChan = 0; // 0:INDIA, 1:GLOBAL
static int curCat  = 0; // 0:TOP, 1:TECH, 2:BIZ, 3:SPORTS, 4:ENTERTAIN, 5:WORLD, 6:SCI, 7:HEALTH

static const char* chanNames[] = { "INDIA", "GLOBAL" };
static const char* catNames[]  = { "general", "technology", "business", "sports", "entertainment", "world", "science", "health" };

static volatile bool busy = false;
static volatile bool haveResult = false;
static volatile bool resultOk = false;
static NewsData resultData;

static TaskHandle_t taskH = nullptr;
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

static String getCachePath(int chan, int cat) {
  char path[40];
  snprintf(path, sizeof(path), "/news_c%d_cat%d.bin", chan, cat);
  return String(path);
}

static void loadCache(int chan, int cat) {
  memset(&cached, 0, sizeof(cached));
  
  // Use specific cache file for each channel and category
  String path = getCachePath(chan, cat);
  File f = LittleFS.open(path, "r");
  if (f) {
    f.read((uint8_t*)&cached, sizeof(cached));
    f.close();
  }

  // Ensure current state matches the requested cache
  curChan = chan;
  curCat = cat;

  if (cached.count <= 0) {
    cached.count = 1;
    cached.index = 0;
    cached.channel = chan;
    cached.category = cat;
    safeStrcpy(cached.title[0], sizeof(cached.title[0]), "No news cached yet.");
    safeStrcpy(cached.desc[0], sizeof(cached.desc[0]), "Please connect to WiFi and fetch news.");
    safeStrcpy(cached.updated, sizeof(cached.updated), "None");
  }

  if (cached.index < 0) cached.index = 0;
  if (cached.index >= cached.count) cached.index = cached.count - 1;
}

static void saveCache(const NewsData& nd) {
  String path = getCachePath(nd.channel, nd.category);
  File f = LittleFS.open(path, "w");
  if (f) {
    f.write((uint8_t*)&nd, sizeof(nd));
    f.close();
  }
}

static bool fetchNewsOnce(NewsData& out) {
  if (WiFi.status() != WL_CONNECTED || !netIsConnected()) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);

  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  String url;
  bool isNewsAPI = false;

  if (!GNEWS_KEY || !GNEWS_KEY[0]) return false;

  if (curChan == 0) {
    // 0: INDIA NEWS (country=in)
    url = "https://gnews.io/api/v4/top-headlines?country=in&lang=en&max=" + String(NEWS_MAX) + 
          "&category=" + String(catNames[curCat]) + "&token=" + String(GNEWS_KEY);
  } else {
    // 1: GLOBAL NEWS (no specific country)
    url = "https://gnews.io/api/v4/top-headlines?lang=en&max=" + String(NEWS_MAX) + 
          "&category=" + String(catNames[curCat]) + "&token=" + String(GNEWS_KEY);
  }

  if (!http.begin(client, url)) return false;
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "NAA_TV/1.0");

  int rc = http.GET();
  String body = http.getString();
  http.end();

  if (rc != 200 || body.length() < 20) return false;

  // Optimized filtering
  StaticJsonDocument<1024> filter;
  JsonArray fArts = filter.createNestedArray("articles");
  for (int i = 0; i < NEWS_MAX; i++) {
    JsonObject o = fArts.createNestedObject();
    o["title"] = true;
    o["description"] = true;
  }

  DynamicJsonDocument doc(16000); 
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;

  JsonArray arts = doc["articles"].as<JsonArray>();
  int n = 0;
  for (JsonVariant v : arts) {
    if (n >= NEWS_MAX) break;
    String t = String((const char*)(v["title"] | ""));
    String d = String((const char*)(v["description"] | ""));
    t.trim();
    if (t.length() == 0) continue;
    
    // NewsAPI often adds source to title like "Title - BBC News", let's clean it
    if (isNewsAPI) {
      int dashIdx = t.lastIndexOf(" - ");
      if (dashIdx > 0) t = t.substring(0, dashIdx);
    }

    safeStrcpy(out.title[n], sizeof(out.title[n]), t);
    safeStrcpy(out.desc[n], sizeof(out.desc[n]), d);
    n++;
  }
  if (n <= 0) return false;

  out.count = n;
  out.index = 0;
  out.channel = curChan;
  out.category = curCat;
  stampNow(out.updated);
  return true;
}

static void newsTask(void*) {
  // Use heap instead of stack to prevent overflow (NewsData is ~11KB)
  NewsData* out = (NewsData*)malloc(sizeof(NewsData));
  if (!out) {
    busy = false;
    vTaskDelete(nullptr);
    return;
  }
  memset(out, 0, sizeof(NewsData));

  bool ok = fetchNewsOnce(*out);
  if (!ok) {
    vTaskDelay(250 / portTICK_PERIOD_MS);
    ok = fetchNewsOnce(*out);
  }
  
  if (dataMutex && xSemaphoreTake(dataMutex, portMAX_DELAY)) {
    if (ok) {
      resultData = *out;
      resultOk = true;
    } else {
      resultOk = false;
    }
    haveResult = true;
    xSemaphoreGive(dataMutex);
  }

  free(out);
  busy = false;
  taskH = nullptr;
  vTaskDelete(nullptr);
}

void newsBegin() { 
  dataMutex = xSemaphoreCreateMutex();
  loadCache(curChan, curCat); 
}
void newsLoop() {}

bool newsIsBusy() { return busy; }

void newsRequestFetch() {
  if (busy) return;
  busy = true;
  haveResult = false;
  resultOk = false;
  xTaskCreatePinnedToCore(newsTask, "newsTask", 10240, nullptr, 1, &taskH, 0);
}

void newsSetChannel(int c) { 
  if (c >= 0 && c < 2) { 
    if (curChan != c) {
      curChan = c; 
      loadCache(curChan, curCat);
    }
  } 
}

void newsSetCategory(int c) { 
  if (c >= 0 && c < 8) { 
    if (curCat != c) {
      curCat = c; 
      loadCache(curChan, curCat);
    }
  } 
}

bool newsConsumeResult(bool& ok, NewsData& out) {
  if (!haveResult) return false;

  if (dataMutex && xSemaphoreTake(dataMutex, portMAX_DELAY)) {
    haveResult = false;
    ok = resultOk;

    if (ok) {
      out = resultData;
      cached = out;
      if (cached.count < 1) cached.count = 1;
      if (cached.index < 0) cached.index = 0;
      if (cached.index >= cached.count) cached.index = cached.count - 1;
      saveCache(cached);
    } else {
      out = cached;
    }
    
    xSemaphoreGive(dataMutex);
  }
  
  return true;
}

const NewsData& newsGetCached() { return cached; }

void newsNext() {
  if (cached.count <= 0) return;
  cached.index++;
  if (cached.index >= cached.count) cached.index = 0;
  saveCache(cached);
}

void newsPrev() {
  if (cached.count <= 0) return;
  cached.index--;
  if (cached.index < 0) cached.index = cached.count - 1;
  saveCache(cached);
}
