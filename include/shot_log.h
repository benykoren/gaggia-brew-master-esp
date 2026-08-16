#pragma once

#include <Arduino.h>

// Append-only shot history on LittleFS. See config.h for the storage
// rationale and record format.

void shotLogInit();

// Appends one shot record, trimming the oldest entries first if the log is
// already at SHOT_LOG_MAX_ENTRIES. `weightGrams` is 0 until a scale exists.
// `endTemp` is the temperature at the moment the shot stopped (0 = unknown,
// same convention as weightGrams) - lets a shot that ended cold/hot show up
// distinctly from one that merely peaked high mid-pull.
void shotLogAppend(time_t timestamp, unsigned long durationMs, float peakTemp,
                   float weightGrams, float endTemp);

// Returns the full log as a JSON array, oldest-first, e.g.:
// [{"ts":1755289200,"duration_ms":28500,"peak_temp":93.4,"weight":0.0,"end_temp":92.1}, ...]
// end_temp reads 0 for older log entries recorded before this field existed.
String shotLogReadJson();
