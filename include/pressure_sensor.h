#pragma once

// Analog pressure transducer driver (HARDWARE_ROADMAP.md item 7).
//
// Reads PIN_PRESSURE_ADC (ADC1 - see config.h for why not ADC2) and converts
// the raw millivolt reading to bar via a linear calibration
// (PRESSURE_SENSOR_ZERO_MV/PRESSURE_SENSOR_MV_PER_BAR in config.h) that must
// be bench-calibrated against the actual transducer part - see the pump-
// pressure bring-up design spec, bring-up Task 11.

enum class PressureSensorStatus { OK, OUT_OF_RANGE };

void pressureSensorInit();

// Reads the ADC, converts to bar, and writes it to `outBar`. Returns
// OUT_OF_RANGE (outBar left at 0) if the raw reading is implausible (below
// the zero-mv floor or well above what a healthy reading on this machine
// could ever be) - almost always a disconnected/failed sensor rather than a
// real reading, since the safety ceiling (PUMP_MAX_SAFETY_BAR) is well
// inside the transducer's actual range.
PressureSensorStatus pressureSensorRead(float &outBar);
