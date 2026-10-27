#pragma once
#include <stdint.h>
#include <stdbool.h>

struct Alarm {
    int8_t hour;     // 0-23
    int8_t minute;   // 0-59
    bool enabled;
};

void alarmsBegin();
void alarmsSave();
Alarm* alarmsGet(int index);
void alarmsUpdate(int index, int8_t h, int8_t m, bool e);

// Check if any alarm should trigger now
bool alarmsCheckTrigger(int8_t h, int8_t m, int8_t s);
void alarmsResetTriggered();
bool alarmsIsTriggered();
