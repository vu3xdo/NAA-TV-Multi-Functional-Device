
#include "sensors.h"
#include "config.h"
#include "net.h"

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <RTClib.h>
#include <time.h>

// =========================
// Global sensor state
// =========================

// BMP280: temperature + pressure
static Adafruit_BMP280 bmp;
static bool  bmpOk      = false;
static bool  bmpAwake   = false; // true only while Temp & Pressure screen is open
static float lastTemp   = NAN;  // °C
static float lastPress  = NAN;  // hPa

// DS3231 RTC
static RTC_DS3231 rtc;
static bool rtcOk   = false;
static bool ntpDone = false;


// =========================
// Init / main loop
// =========================

void sensorsBegin() {
    // I2C for BMP280 + RTC
    Wire.begin(I2C_SDA, I2C_SCL);

    // BMP280 init
    bmpOk = bmp.begin(BMP_ADDR_1);    // 0x76
    if (!bmpOk) {
        bmpOk = bmp.begin(BMP_ADDR_2); // 0x77
    }

    if (bmpOk) {
        // Start in SLEEP mode — will only wake when Temp & Pressure screen opens
        bmp.setSampling(Adafruit_BMP280::MODE_SLEEP,
                        Adafruit_BMP280::SAMPLING_NONE,
                        Adafruit_BMP280::SAMPLING_NONE,
                        Adafruit_BMP280::FILTER_OFF,
                        Adafruit_BMP280::STANDBY_MS_1000);
        bmpAwake = false;
        Serial.println("BMP280 init OK (sleeping until Temp screen)");
    } else {
        Serial.println("BMP280 init FAIL");
    }

    // DS3231 RTC
    rtcOk = rtc.begin();
    if (rtcOk) {
        Serial.println("RTC init OK");
        if (rtc.lostPower()) {
            Serial.println("RTC lost power, will sync from NTP");
        }
    } else {
        Serial.println("RTC init FAIL");
    }
}



// =========================
// TIME / RTC helpers
// =========================

bool sensorsRtcOk() {
    return rtcOk;
}

RTC_DS3231* sensorsRtcPtr() {
    return rtcOk ? &rtc : nullptr;
}

DateTime sensorsNow() {
    if (!sensorsRtcOk()) return DateTime(0UL);
    // PRO UPGRADE: Produce local time by adding offset to True UTC stored in RTC
    return DateTime(rtc.now().unixtime() + IST_OFFSET_SEC);
}

void stampNow(char *out, size_t cap) {
    if (!sensorsRtcOk()) {
        strlcpy(out, "NO TIME", cap);
        return;
    }
    // PRO UPGRADE: Use the offset-aware timezone function
    DateTime dt = sensorsNow();
    int hh = dt.hour();
    bool pm = (hh >= 12);
    int hh12 = hh % 12;
    if (hh12 == 0) hh12 = 12;
    snprintf(out, cap, "%02d:%02d %s", hh12, dt.minute(), pm ? "PM" : "AM");
}


// =========================
// Temperature / Pressure
// =========================

static uint32_t lastBmpRead = 0;

// Wake BMP280 from sleep — call when Temp & Pressure screen opens.
// The sensor needs ~200 ms to take its first stable measurement.
void sensorsWakeBMP() {
    if (!bmpOk) return;
    if (bmpAwake) return;               // already awake, don't re-init
    // Discard any stale cached values so first reading is fresh
    lastTemp  = NAN;
    lastPress = NAN;
    
    // PRO UPGRADE: Use FORCED mode. The chip takes exactly one high-precision
    // measurement and instantly drops into deep sleep mode, eliminating self-heating.
    bmp.setSampling(Adafruit_BMP280::MODE_FORCED,
                    Adafruit_BMP280::SAMPLING_X2,   // temp
                    Adafruit_BMP280::SAMPLING_X16,  // pressure (max precision)
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_1);
    bmpAwake = true;
    lastBmpRead = 0; // force immediate read next call
    Serial.println("BMP280 waking up for Temp screen (Forced Mode)");
}

// Put BMP280 back to sleep — call when leaving Temp & Pressure screen.
void sensorsIdleBMP() {
    if (!bmpOk) return;
    if (!bmpAwake) return;              // already sleeping
    bmp.setSampling(Adafruit_BMP280::MODE_SLEEP,
                    Adafruit_BMP280::SAMPLING_NONE,
                    Adafruit_BMP280::SAMPLING_NONE,
                    Adafruit_BMP280::FILTER_OFF,
                    Adafruit_BMP280::STANDBY_MS_1000);
    bmpAwake  = false;
    // Clear cached readings so next wake starts fresh
    lastTemp  = NAN;
    lastPress = NAN;
    Serial.println("BMP280 put to sleep");
}

bool sensorsIsBMPAwake() { return bmpAwake; }

// Temperature from BMP280 only (Celsius).
// Returns NAN while sensor is sleeping (not on Temp screen).
float sensorsTempC() {
    if (!bmpOk || !bmpAwake) return NAN;

    // PRO UPGRADE: Throttle forced reads to 1 per second. 
    // The previous MODE_NORMAL overheated the IC. This acts icy cool.
    if (millis() - lastBmpRead > 1000 || lastBmpRead == 0) {
        if (bmp.takeForcedMeasurement()) {
            lastTemp = bmp.readTemperature();
            float p = bmp.readPressure(); // Pa
            if (!isnan(p) && p > 10000.0f) lastPress = p / 100.0f; // Pa directly to hPa
        } else {
            Serial.println("BMP280 forced read failed");
        }
        lastBmpRead = millis();
    }
    return lastTemp;
}

// Humidity not used (no DHT11 connected)
float sensorsHumidity() {
    return NAN;
}

// Pressure from BMP280 (hPa).
// Returns NAN while sensor is sleeping.
float sensorsPressureHPa() {
    if (!bmpOk || !bmpAwake) return NAN;

    // Utilize the same cached read logic so Pressure doesn't hammer the I2C bus twice
    if (millis() - lastBmpRead > 1000 || lastBmpRead == 0) {
        if (bmp.takeForcedMeasurement()) {
            lastTemp = bmp.readTemperature();
            float p = bmp.readPressure(); 
            if (!isnan(p) && p > 10000.0f) lastPress = p / 100.0f;
        } else {
            Serial.println("BMP280 forced read failed");
        }
        lastBmpRead = millis();
    }
    return lastPress;
}


// =========================
// NTP → RTC sync (IST)
// =========================

void sensorsStep5NtpRtcLoop() {
    if (ntpDone) return;
    if (!netIsConnected()) return;
    if (!sensorsRtcOk()) return;

    static bool started = false;
    static uint32_t startAt = 0;

    if (!started) {
        started = true;
        startAt = millis();
        // PRO UPGRADE: Fetch pure UTC time (0 offset) from servers.
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        setenv("TZ", "IST-5:30", 1); // Native ESP32 timezone handling
        tzset();
        Serial.println("NTP started in UTC mode");
    }

    time_t now = 0;
    time(&now);

    if (now > 1700000000UL) {
        // PRO UPGRADE: Store pristine true UTC inside the hardware DS3231
        DateTime dt_utc(now);
        rtc.adjust(dt_utc);
        ntpDone = true;

        char buf[20];
        stampNow(buf, sizeof(buf));    // safely applies local shift to display
        Serial.print("RTC successfully synced to purely UTC time. Current Local Time: ");
        Serial.println(buf);
        return;
    }
}

// Global clock tick to force re-sync RTC every 24 hours
void sensorsLoop() {
    sensorsStep5NtpRtcLoop();
    
    // Auto-resync hardware RTC with internet time every 24 hours to prevent crystal drift
    static uint32_t lastDailySync = 0;
    if (ntpDone && (millis() - lastDailySync > 86400000UL)) {
        lastDailySync = millis();
        ntpDone = false; // Triggers the NTP -> RTC copy at the next available internet time
    }
}
