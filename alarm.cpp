#include "alarm.h"
#include "storage.h"

// Max 3 alarms
static const int ALARM_MAX = 3;
static Alarm alarms[ALARM_MAX];
static bool triggered = false;

void alarmsBegin() {
    for (int i = 0; i < ALARM_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "al_h_%d", i);
        int8_t h = storageGetI32(key, -1);
        
        snprintf(key, sizeof(key), "al_m_%d", i);
        int8_t m = storageGetI32(key, 0);

        snprintf(key, sizeof(key), "al_e_%d", i);
        bool e = storageGetBool(key, false);

        if (h >= 0 && h <= 23) {
            alarms[i].hour = h;
            alarms[i].minute = m;
            alarms[i].enabled = e;
        } else {
            alarms[i].hour = 6;
            alarms[i].minute = 0;
            alarms[i].enabled = false;
        }
    }
}

void alarmsSave() {
    for (int i = 0; i < ALARM_MAX; i++) {
        char key[16];
        snprintf(key, sizeof(key), "al_h_%d", i);
        storageSetI32(key, alarms[i].hour);
        
        snprintf(key, sizeof(key), "al_m_%d", i);
        storageSetI32(key, alarms[i].minute);

        snprintf(key, sizeof(key), "al_e_%d", i);
        storageSetBool(key, alarms[i].enabled);
    }
}

Alarm* alarmsGet(int index) {
    if (index < 0 || index >= ALARM_MAX) return nullptr;
    return &alarms[index];
}

void alarmsUpdate(int index, int8_t h, int8_t m, bool e) {
    if (index < 0 || index >= ALARM_MAX) return;
    alarms[index].hour = h;
    alarms[index].minute = m;
    alarms[index].enabled = e;
    alarmsSave();
}

bool alarmsCheckTrigger(int8_t h, int8_t m, int8_t s) {
    // Check if any alarm should trigger precisely at minute boundary (s == 0)
    for (int i = 0; i < ALARM_MAX; i++) {
        if (alarms[i].enabled && alarms[i].hour == h && alarms[i].minute == m && s == 0) {
            triggered = true;
            alarms[i].enabled = false; // Auto disable upon triggering
            alarmsSave();
            return true;
        }
    }
    return false;
}

void alarmsResetTriggered() {
    triggered = false;
}

bool alarmsIsTriggered() {
    return triggered;
}
