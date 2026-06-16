#!/usr/bin/env python3
"""
bench-serial-capture.py — capture a board's serial output for the v8.4 bench.

Opens the given COM port, hardware-resets the board via DTR/RTS (so the one-time boot
config dump is captured), and prints everything the board emits for N seconds. This is
how the boot dumps and `evt=...` event lines in BENCH-RESULTS.md were obtained.

Manual equivalent: `pio device monitor --port COMx --baud 115200`, then press the
board's RST button.

Requires: pip install pyserial
Usage:    python tools/bench-serial-capture.py <COMx> [seconds]
Example:  python tools/bench-serial-capture.py COM13 90
"""
import sys, time
try:
    import serial
except ImportError:
    sys.exit("error: pip install pyserial")

port = sys.argv[1] if len(sys.argv) > 1 else "COM13"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 9.0

ser = serial.Serial(port, 115200, timeout=0.2)
# Classic auto-reset to RUN firmware (not bootloader): GPIO0 high, pulse EN.
try:
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.15)
    ser.setDTR(True);  ser.setRTS(False); time.sleep(0.15)
    ser.setDTR(False)
except Exception as e:
    sys.stderr.write("(reset toggle failed: %s)\n" % e)

end = time.time() + secs
while time.time() < end:
    data = ser.readline()
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
ser.close()
