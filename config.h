
#pragma once

// ====================== BUILD STEP ======================
#define BUILD_STEP 10

// ====================== DISPLAY PINS ======================
#define TFT_CS   10
#define TFT_DC   9
#define TFT_RST  14
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_SCK  12
#define TFT_BL   21

// ====================== TOUCH PINS ======================
#define TOUCH_CS   7
#define TOUCH_IRQ  8

// ====================== I2C PINS ======================
#define I2C_SDA  15
#define I2C_SCL  16

// ====================== ENCODER PINS ======================
#define ENC_A   4
#define ENC_B   5
#define ENC_SW  6

// ====================== SCREEN GEOMETRY ======================
#define TFT_ROTATION 1
#define TFT_W 320
#define TFT_H 240

// ====================== TOUCH ROTATION ======================
#define TOUCH_ROTATION 3

// ====================== TOUCH DEFAULT CAL ======================
#define TS_MIN_X 47
#define TS_MAX_X 3721
#define TS_MIN_Y 286
#define TS_MAX_Y 3896

// Touch behavior tuning
#define TOUCH_DEBOUNCE_MS     35
#define TOUCH_LONGPRESS_MS    700
#define TOUCH_SAMPLE_THROTTLE 25

// Gesture thresholds
#define TOUCH_SWIPE_MIN_PX     70
#define TOUCH_SWIPE_MAX_OFFPX  45
#define TOUCH_TAP_MAX_MOVE_PX  18
#define TOUCH_TAP_MAX_MS       240

#define TOUCH_FORCE_INV_X (-1)
#define TOUCH_FORCE_INV_Y (-1)

// ====================== POWER / IDLE ======================
// "Soft sleep": backlight off after idle, wake on touch IRQ / encoder / joystick.
// We intentionally DO NOT enter deep sleep so WiFi can remain connected.
#ifndef POWER_IDLE_BACKLIGHT_MS
  #define POWER_IDLE_BACKLIGHT_MS 60000UL  // 60s idle -> backlight off
#endif

#ifndef POWER_WAKE_DEBOUNCE_MS
  #define POWER_WAKE_DEBOUNCE_MS 150UL     // ignore repeated wake edges
#endif

// NEW: Display timeout options for Settings
#define TIMEOUT_15S  15000UL
#define TIMEOUT_30S  30000UL
#define TIMEOUT_45S  45000UL
#define TIMEOUT_60S  60000UL
#define TIMEOUT_120S 120000UL
#define TIMEOUT_NEVER 0xFFFFFFFFUL

// Face Animation Constants
#define FACE_ANIM_INTERVAL_MS 50
#define FACE_BREATH_PERIOD_MS 3000

// ====================== WIFI TUNING ======================
#define WIFI_RETRY_MS 5000
#define NET_TIMEOUT_MS 6500

// ====================== TIMEZONE ======================
#define IST_OFFSET_SEC 19800

// ====================== BMP280 ======================
#define BMP_ADDR_1 0x76
#define BMP_ADDR_2 0x77

// ====================== JOYSTICK (for GAMES) ======================
// IMPORTANT: This mirrors the NT_ approach: compile-time defaults that you can override.
// If you already have correct pins/centers from your joystick test, set them here.
#ifndef JOY_X_PIN
  #define JOY_X_PIN   1
#endif
#ifndef JOY_Y_PIN
  #define JOY_Y_PIN   2
#endif
#ifndef JOY_BTN_PIN
  #define JOY_BTN_PIN 3
#endif

#ifndef JOY_ADC_ATTEN
  #define JOY_ADC_ATTEN ADC_11db
#endif
#ifndef JOY_ADC_SAMPLES
  #define JOY_ADC_SAMPLES 8
#endif

#ifndef JOY_CENTER_X
  #define JOY_CENTER_X 1885
#endif
#ifndef JOY_CENTER_Y
  #define JOY_CENTER_Y 1855
#endif
#ifndef JOY_DEADZONE
  #define JOY_DEADZONE 450
#endif

