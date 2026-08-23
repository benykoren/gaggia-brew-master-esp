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

// Splits a stored line into its first 5 fields (ts,duration_ms,peak_temp,
// weight,end_temp) as raw substrings - preserved byte-for-byte rather than
// reparsed to numbers and reformatted, so shotLogUpdateNotes() rewriting a
// line's metadata never risks altering the measurement fields it isn't
// touching. Returns each field plus how much of `line` those 5 fields
// consumed (so the caller can find where any trailing metadata starts).
// end_temp is backfilled to "0.0" for old 4-field rows that predate it.
struct BaseFields {
  String ts, dur, peak, weight, endTemp;
  bool ok = false;
};

static BaseFields parseBaseFields(const String &line) {
  BaseFields r;
  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  int c3 = line.indexOf(',', c2 + 1);
  if (c1 < 0 || c2 < 0 || c3 < 0) return r;  // malformed

  int c4 = line.indexOf(',', c3 + 1);  // end_temp is optional (see above)
  r.ts = line.substring(0, c1);
  r.dur = line.substring(c1 + 1, c2);
  r.peak = line.substring(c2 + 1, c3);
  if (c4 < 0) {
    r.weight = line.substring(c3 + 1);
    r.endTemp = "0.0";
  } else {
    int c5 = line.indexOf(',', c4 + 1);  // metadata fields, if already present
    r.weight = line.substring(c3 + 1, c4);
    r.endTemp = (c5 < 0) ? line.substring(c4 + 1) : line.substring(c4 + 1, c5);
  }
  r.ok = true;
  return r;
}

// Splits off whatever comes after the first 5 fields (the tasting/dial-in
// metadata block: bean,dose_in,grind,rating,notes) - "" if the row predates
// that block entirely, matching parseBaseFields()'s own field-counting.
static String metadataFields(const String &line) {
  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  int c3 = line.indexOf(',', c2 + 1);
  int c4 = line.indexOf(',', c3 + 1);
  if (c4 < 0) return "";
  int c5 = line.indexOf(',', c4 + 1);
  if (c5 < 0) return "";
  return line.substring(c5 + 1);
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

      BaseFields b = parseBaseFields(line);
      if (!b.ok) continue;  // malformed line, skip

      // Metadata (bean/dose/grind/rating/notes) - "" for rows that predate
      // this block, same optional-trailing-fields pattern as end_temp above.
      String meta = metadataFields(line);
      String bean = "", grind = "", notes = "";
      float doseIn = 0.0f;
      int rating = 0;
      if (meta.length() > 0) {
        int m1 = meta.indexOf(',');
        int m2 = meta.indexOf(',', m1 + 1);
        int m3 = meta.indexOf(',', m2 + 1);
        int m4 = meta.indexOf(',', m3 + 1);
        if (m1 >= 0 && m2 >= 0 && m3 >= 0 && m4 >= 0) {
          bean = meta.substring(0, m1);
          doseIn = meta.substring(m1 + 1, m2).toFloat();
          grind = meta.substring(m2 + 1, m3);
          rating = meta.substring(m3 + 1, m4).toInt();
          notes = meta.substring(m4 + 1);
        }
      }

      if (!first) json += ",";
      first = false;
      json += "{\"ts\":" + b.ts + ",\"duration_ms\":" + b.dur +
              ",\"peak_temp\":" + b.peak + ",\"weight\":" + b.weight +
              ",\"end_temp\":" + b.endTemp + ",\"bean\":\"" + bean +
              "\",\"dose_in\":" + String(doseIn, 1) + ",\"grind\":\"" + grind +
              "\",\"rating\":" + String(rating) + ",\"notes\":\"" + notes + "\"}";
    }
    f.close();
  }

  json += "]";
  return json;
}

void shotLogUpdateNotes(int index, String bean, float doseInGrams, String grind,
                        int rating, String notes) {
  // CSV storage, no quoting support - keep it simple (same simplification
  // profiles.cpp already makes for profile names).
  bean.replace(",", ""); bean.replace("\n", "");
  grind.replace(",", ""); grind.replace("\n", "");
  notes.replace(",", ""); notes.replace("\n", "");
  rating = constrain(rating, 0, 5);

  File src = LittleFS.open(SHOT_LOG_PATH, "r");
  if (!src) return;
  String lines[SHOT_LOG_MAX_ENTRIES];
  int count = 0;
  while (src.available() && count < SHOT_LOG_MAX_ENTRIES) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    lines[count++] = line;
  }
  src.close();

  if (index < 0 || index >= count) return;
  BaseFields b = parseBaseFields(lines[index]);
  if (!b.ok) return;  // malformed, don't touch

  lines[index] = b.ts + "," + b.dur + "," + b.peak + "," + b.weight + "," + b.endTemp +
                 "," + bean + "," + String(doseInGrams, 1) + "," + grind + "," +
                 String(rating) + "," + notes;

  File dst = LittleFS.open(SHOT_LOG_PATH, "w");
  if (!dst) {
    Serial.println("shotLogUpdateNotes: failed to open log file for rewrite");
    return;
  }
  for (int i = 0; i < count; i++) dst.print(lines[i] + "\n");
  dst.close();
}
