// ============================================================================
// Temperature-module protocol prober  (reverse-engineering helper) - v7
// ----------------------------------------------------------------------------
// STANDALONE diagnostic (compiled only by the `esp32-s3-sniffer` env).
//
// BREAKTHROUGH (from the user's OWN serial-terminal test, independent of this
// firmware): sending "AT+T" repeatedly returns:
//     OK
//     +T=28.76
//     OK
//     +T=28.65
//     ...
// i.e. the real read command is "AT+T" and the reply is two lines: "OK" then
// "+T=<value>" (temperature in deg C, PT100 cooling toward ambient in that
// capture - no heater on, makes sense).
//
// BUT v6 (this same relay wiring, flush-then-send strategy) sent a cleanly
// isolated "AT+T\n" and got ERROR_1 back, not OK. Two candidate explanations:
//   1) Wrong terminator - v6 used bare LF; the user's tool may default to
//      CRLF. v7 tests both, back to back, to settle it.
//   2) The v6 flushLine() trick (sending an empty "\n" before every real
//      command) may itself be upsetting the module's parser state (recall:
//      the module's own error text is literally "Too much enter") - so v7
//      does NOT send any blank/flush line at all. It sends ONLY "AT+T" (with
//      each terminator variant), back-to-back, exactly like a human typing
//      it repeatedly into a terminal, to match the working case as closely
//      as possible.
//
// Wiring unchanged: module 5V->ESP 3V3, GND->GND, module TX->GPIO18 (ESP RX),
// module RX->GPIO17 (ESP TX). Read via tools/serial_capture.py COM6 115200.
// Power-cycle (unplug/replug) before capturing so the module starts fresh.
// ============================================================================
#include <Arduino.h>

HardwareSerial ModSerial(1); // UART1
static const int PIN_RX = 18;
static const int PIN_TX = 17;
static const uint32_t BAUD = 9600;

static void drainPrint(uint32_t ms) {
  uint32_t t = millis();
  int seen = 0;
  while ((millis() - t) < ms) {
    while (ModSerial.available()) {
      uint8_t b = (uint8_t)ModSerial.read();
      if (seen == 0) Serial.print("  <= ");
      if (b >= 32 && b < 127) Serial.printf("%02X(%c) ", b, (char)b);
      else Serial.printf("%02X ", b);
      seen++;
    }
  }
  if (seen == 0) Serial.print("  <= (no reply)");
  Serial.println();
}

// Send ONLY the command + terminator - no pre-flush, no other traffic.
static void sendOnly(const char *cmd, const char *term, const char *termName, uint32_t waitMs) {
  Serial.printf("\n[CMD] \"%s\"+%s ->", cmd, termName);
  ModSerial.print(cmd);
  if (term[0]) ModSerial.print(term);
  ModSerial.flush();
  drainPrint(waitMs);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("==================================================");
  Serial.println(" Protocol prober v7 @ 9600 8N1 (RX=18 TX=17)");
  Serial.println(" AT+T only, both terminators, NO pre-flush noise");
  Serial.println("==================================================");
  ModSerial.begin(BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  delay(300);
}

void loop() {
  Serial.println("\n===== Phase A: \"AT+T\" + CRLF, x8, 1s apart =====");
  for (int i = 0; i < 8; i++) {
    sendOnly("AT+T", "\r\n", "CRLF", 1000);
  }

  Serial.println("\n===== Phase B: \"AT+T\" + bare LF, x8, 1s apart =====");
  for (int i = 0; i < 8; i++) {
    sendOnly("AT+T", "\n", "LF", 1000);
  }

  Serial.println("\n===== v7 cycle complete; pause 5s before repeating =====");
  delay(5000);
}
