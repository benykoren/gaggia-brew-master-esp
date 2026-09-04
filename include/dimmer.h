#pragma once

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
// any task - the target is a plain volatile float, not behind main.cpp's
// stateMutex, because it's also read from the zero-cross ISR, where taking
// a FreeRTOS mutex isn't safe.
void dimmerSetPowerPercent(float percent);

// Returns the last percent passed to dimmerSetPowerPercent(), for bumpless
// stage transitions and /status reporting.
float dimmerGetPowerPercent();
