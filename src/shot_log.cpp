#include "shot_log.h"

#include <LittleFS.h>

#include "config.h"

void shotLogInit() {
  if (!LittleFS.begin(true)) {
    Serial.println("shotLogInit: LittleFS mount failed even after format");
  }
}

static int countLines(File &f) {
  int count = 0;
  while (f.available()) {
    if (f.readStringUntil('\n').length() > 0) count++;
  }
  return count;
}

void shotLogAppend(time_t timestamp, unsigned long durationMs, float peakTemp,
                   float weightGrams, float endTemp) {
  int lineCount = 0;
  File existing = LittleFS.open(SHOT_LOG_PATH, "r");
  if (existing) {
    lineCount = countLines(existing);
    existing.close();
  }

  // Trim just enough oldest entries to make room, so the log never grows
  // past SHOT_LOG_MAX_ENTRIES once the new record is appended below.
  if (lineCount >= SHOT_LOG_MAX_ENTRIES) {
    int toDrop = lineCount - SHOT_LOG_MAX_ENTRIES + 1;
    String kept;
    File src = LittleFS.open(SHOT_LOG_PATH, "r");
    if (src) {
      int skipped = 0;
      while (src.available()) {
        String line = src.readStringUntil('\n');
        if (line.length() == 0) continue;
        if (skipped < toDrop) {
          skipped++;
          continue;
        }
        kept += line + "\n";
      }
      src.close();
    }
    File dst = LittleFS.open(SHOT_LOG_PATH, "w");
    if (dst) {
      dst.print(kept);
      dst.close();
    }
  }

  File f = LittleFS.open(SHOT_LOG_PATH, "a");
  if (!f) {
    Serial.println("shotLogAppend: failed to open log file for append");
    return;
  }
  f.printf("%lld,%lu,%.1f,%.1f,%.1f\n", (long long)timestamp, durationMs,
            peakTemp, weightGrams, endTemp);
  f.close();
}

String shotLogReadJson() {
  String json = "[";
  bool first = true;

  File f = LittleFS.open(SHOT_LOG_PATH, "r");
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      int c1 = line.indexOf(',');
      int c2 = line.indexOf(',', c1 + 1);
      int c3 = line.indexOf(',', c2 + 1);
      if (c1 < 0 || c2 < 0 || c3 < 0) continue;  // malformed line, skip

      // c4 (end_temp) is optional - older rows written before this field
      // existed only have 4 columns, so its absence isn't malformed.
      int c4 = line.indexOf(',', c3 + 1);

      String ts = line.substring(0, c1);
      String dur = line.substring(c1 + 1, c2);
      String peak = line.substring(c2 + 1, c3);
      String wt = (c4 < 0) ? line.substring(c3 + 1) : line.substring(c3 + 1, c4);
      String endTemp = (c4 < 0) ? "0.0" : line.substring(c4 + 1);

      if (!first) json += ",";
      first = false;
      json += "{\"ts\":" + ts + ",\"duration_ms\":" + dur +
              ",\"peak_temp\":" + peak + ",\"weight\":" + wt +
              ",\"end_temp\":" + endTemp + "}";
    }
    f.close();
  }

  json += "]";
  return json;
}
