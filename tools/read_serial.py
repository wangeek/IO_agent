"""Drain the ESP32 UART0 log on COM3 for a fixed window.

Pure ASCII on purpose: this file is also read by Windows PowerShell 5.1
tooling that assumes the ANSI code page.
"""
import sys
import time

try:
    import serial
except ImportError:
    print("NO_PYSERIAL")
    sys.exit(3)

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

# Opening COM3 with DTR/RTS asserted RESETS the board: the devkit's
# auto-reset circuit wires RTS -> EN and pulls EN low. pyserial asserts both
# lines by default, so the plain open below reboots the MCU and every log
# window starts with a full boot banner.
#
# Fix: de-assert DTR/RTS BEFORE open() (the constructor opens immediately),
# then set them again after open() for drivers that re-assert on open.
#
# Pass --reset as argv[3] if you actually WANT a power cycle.
do_reset = len(sys.argv) > 3 and sys.argv[3] == "--reset"

try:
    if do_reset:
        ser = serial.Serial(port, 115200, timeout=0.25)
    else:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 0.25
        ser.dtr = False
        ser.rts = False
        ser.open()
        ser.dtr = False
        ser.rts = False
except Exception as exc:  # noqa: BLE001
    print("OPEN_FAILED: {}".format(exc))
    sys.exit(2)

print("OPENED {} @115200, reading {:.1f}s".format(port, seconds))
deadline = time.time() + seconds
total = 0
while time.time() < deadline:
    try:
        chunk = ser.read(4096)
    except Exception as exc:  # noqa: BLE001
        print("READ_ERROR: {}".format(exc))
        break
    if chunk:
        total += len(chunk)
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()

ser.close()
print("\n--- drained {} bytes ---".format(total))
