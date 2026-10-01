#pragma once

#include <stdint.h>

// AC dimmer driver (HARDWARE_ROADMAP.md item 8) - zero-cross detection +
// TRIAC gate firing via a hardware one-shot timer, so firing timing survives
// WiFi/BT scheduling jitter (a plain delayMicroseconds() in the zero-cross
// ISR does not - see config.h). Power is pulse-skip modulation (PSM): whole
// mains cycles are either fired near the zero-cross or skipped, which is
// what a diode-fed vibration pump actually responds to (see config.h's
// PUMP_BREW_POWER_PCT_DEFAULT comment).

// Configures PIN_DIMMER_ZC as an interrupt input and PIN_DIMMER_GATE as an
// output, and creates the internal esp_timer used for gate firing. Call
// once from setup().
void dimmerInit();

// Sets the target power level (0-100), clamped to that range. 0 = TRIAC
// never fires (pump off); 100 = every mains cycle fired (full pass-through);
// N = N% of mains cycles fired, spread evenly (Bresenham accumulator). Safe to call from
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
