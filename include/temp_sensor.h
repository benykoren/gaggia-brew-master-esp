#pragma once

// UART PT100 module driver.
//
// Talks to the "MAX31865-labeled" temperature/relay module over its own
// serial line (NOT SPI - the silkscreen is misleading, it's an ASCII
// AT-command UART module). Protocol reverse-engineered on the bench:
//   TX: "AT+T\r\n"
//   RX: "+T=<value>\r\n" followed by "OK\r\n" on success, "ERROR_1\r\n" on a
//       malformed/unrecognized command.
// See AGENTS.md Section 9 (Change Log) for the discovery process.

enum class TempSensorStatus { OK, TIMEOUT, ERROR_REPLY };

void tempSensorInit();

// Sends AT+T and waits for a reply. On success, writes the parsed
// temperature (deg C) to `outC` and returns OK. Returns TIMEOUT if no
// recognizable reply arrived in time, or ERROR_REPLY if the module answered
// with ERROR_1 (or anything else unparseable) - both are sensor faults.
TempSensorStatus tempSensorRead(float &outC);
