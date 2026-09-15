#pragma once

#include <stdint.h>

// AC phase-control dimmer driver (HARDWARE_ROADMAP.md item 8) - zero-cross
// detection + TRIAC gate firing via a hardware one-shot timer, so firing
// timing survives WiFi/BT scheduling jitter (a plain delayMicroseconds() in
// the zero-cross ISR does not - see config.h).

// Configures PIN_DIMMER_ZC as an interrupt input and PIN_DIMMER_GATE as an
// output, and creates the internal esp_timer used for gate firing. Call
// once from setup().
void dimmerInit();

// Sets the target power level (0-100), clamped to that range. 0 = TRIAC
// never fires (pump off); 100 = fires as close to the zero-cross as
// DIMMER_MIN_FIRING_DELAY_US allows (full pass-through). Safe to call from
// any task - internally stored as a plain volatile integer (tenths of a
// percent), not behind main.cpp's stateMutex, because it's also read from
// the zero-cross ISR, where taking a FreeRTOS mutex isn't safe (and where
// float math isn't safe either - see dimmer.cpp's ISR-safety note).
void dimmerSetPowerPercent(float percent);

// Returns the last percent passed to dimmerSetPowerPercent(), for bumpless
// stage transitions and /status reporting.
float dimmerGetPowerPercent();

// Temporary bring-up diagnostic (2026-09-15) - count of zero-cross
// interrupts actually accepted since boot, so /status can expose a rate
// (crossings/sec) to confirm the ESP32 is really seeing a continuous
// ~100Hz signal from the dimmer's Z-C output. Remove once real hardware
// bring-up is complete and this is no longer needed.
uint32_t dimmerGetZcCount();
