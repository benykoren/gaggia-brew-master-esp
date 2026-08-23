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
// [{"ts":1755289200,"duration_ms":28500,"peak_temp":93.4,"weight":0.0,"end_temp":92.1,
//   "bean":"","dose_in":0.0,"grind":"","rating":0,"notes":""}, ...]
// end_temp/bean/dose_in/grind/rating/notes all read as their "unset" default
// (0 or "") for older log entries recorded before that field existed.
String shotLogReadJson();

// Updates the tasting/dial-in metadata for shot `index` (0-based, oldest-
// first - matches the JSON array's own order, NOT the Web UI's reversed
// display order). See config.h for why this is a separate call rather than
// captured at shotLogAppend() time: you don't know the bean/dose/rating
// until after tasting the coffee, often well after the shot itself. Commas
// and newlines in free-text fields are stripped (CSV storage, no quoting
// support - same simplification profiles.cpp already makes for names).
// No-op if index is out of range.
void shotLogUpdateNotes(int index, String bean, float doseInGrams, String grind,
                        int rating, String notes);
