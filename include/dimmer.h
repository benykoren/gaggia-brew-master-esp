#pragma once

#include <stdint.h>

// AC dimmer driver (HARDWARE_ROADMAP.md item 8). Pulse-skip modulation:
// whole mains cycles are fired or skipped (Bresenham-spread), which is what
// a diode-fed ULKA pump responds to - one stroke per fired cycle. Hardened
// 2026-10-02 (design spec §5): long gate hold via a gptimer, mains
// frequency auto-detect + zero-cross watchdog, stroke counters, slew limit.

enum class MainsState { OK, ABSENT, UNSTABLE };

void dimmerInit();

// Target power 0-100 (clamped). Rises toward it at DIMMER_SLEW_PCT_PER_S
// (advanced by dimmerTick()); any decrease, including 0, applies instantly.
void dimmerSetPowerPercent(float percent);
// Same, but with no slew - used only by the pump calibration run.
void dimmerSetPowerPercentImmediate(float percent);
// Call every control tick: advances the slew and updates mains health.
void dimmerTick(uint32_t dtMs);

// The percent actually being fired right now (after slew).
float dimmerGetPowerPercent();

// Diagnostic: gate hold time in us (clamped 500-8000), not persisted.
void dimmerSetGateHoldUs(int32_t us);
int32_t dimmerGetGateHoldUs();

uint32_t dimmerGetZcCount();     // accepted zero-crossings since boot
uint32_t dimmerGetFiredCycles(); // fired mains cycles (= pump strokes) since boot
MainsState dimmerGetMainsState();
float dimmerGetMainsHz();        // last valid measurement; DIMMER_MAINS_HZ_DEFAULT until then
const char *mainsStateName(MainsState s);
