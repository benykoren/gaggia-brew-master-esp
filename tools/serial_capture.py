"""Self-closing serial capture helper.

Opens a COM port, reads for a fixed number of seconds, prints everything it
receives, then closes the port and exits on its own. This deliberately avoids a
long-lived `pio device monitor`, which (on this Windows setup) could keep holding
the port and jam the agent's terminal when it had to be killed.

Usage:  python serial_capture.py [PORT] [BAUD] [SECONDS]
Default: COM6 115200 45
"""
import sys
import time

try:
    import serial  # pyserial (ships with PlatformIO's penv)
except ImportError:
    print("pyserial not available in this interpreter", flush=True)
    sys.exit(2)

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
duration = float(sys.argv[3]) if len(sys.argv) > 3 else 45.0

ser = serial.Serial()
ser.port = port
ser.baudrate = baud
ser.timeout = 0.2
# Do not toggle the reset/boot lines when we open the port.
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

ser.open()
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

print(f"[capture] {port} @ {baud} for {duration:.0f}s", flush=True)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
finally:
    ser.close()
print("\n[capture done]", flush=True)
