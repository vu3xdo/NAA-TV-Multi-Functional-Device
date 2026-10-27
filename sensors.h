#ifndef SENSORS_H
#define SENSORS_H
#include <stdbool.h>
#include <stdint.h>

#include <RTClib.h>

void sensorsBegin();
void sensorsLoop();

void sensorsStep5NtpRtcLoop();
void sensorsPrintTime();
void sensorsPrintBmp();

// ===== TIME screen support =====
bool sensorsRtcOk();
DateTime sensorsNow();     // safe: returns RTC time (or compile-time fallback if RTC missing)

// ===== NEW: expose RTC pointer for other modules (weather) =====
// This is the SAME RTC instance used by sensorsNow()/TIME screen.
RTC_DS3231* sensorsRtcPtr();

float sensorsTempC();
float sensorsHumidity();
float sensorsPressureHPa();

// BMP280 sleep/wake control (call these when entering/leaving Temp & Pressure screen)
void sensorsWakeBMP();      // wake sensor, clear stale cache
void sensorsIdleBMP();      // put sensor back to sleep
bool sensorsIsBMPAwake();   // true while Temp screen is open

#endif // SENSORS_H
