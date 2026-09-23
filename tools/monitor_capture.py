#!/usr/bin/env python3
"""Reset the board on a serial port and capture N seconds of output.

`idf.py monitor` cannot be scripted (it owns the terminal), so this uses
pyserial directly: assert RTS to pull the board's EN line low, release it, then
stream whatever the ROM/bootloader/app print.

On the ESP32-S31-Korvo the CP210x is wired EN<-RTS, GPIO0<-DTR, so DTR must stay
False to boot normally instead of entering the ROM downloader.

Usage:
    python tools/monitor_capture.py --port COM34 --seconds 30 --out build/boot.log
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required (pip install pyserial)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM34")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=25.0, help="capture window")
    parser.add_argument("--out", default=None, help="also write the log to this file")
    parser.add_argument(
        "--no-reset",
        action="store_true",
        help="only listen, do not reset the board first",
    )
    args = parser.parse_args()

    port = serial.Serial(args.port, args.baud, timeout=0.5)
    sink = open(args.out, "w", encoding="utf-8") if args.out else None

    if not args.no_reset:
        # EN low -> reset; DTR low keeps GPIO0 high so we boot the application.
        port.setDTR(False)
        port.setRTS(True)
        time.sleep(0.15)
        port.setRTS(False)
        # The ROM prints its banner ~50 ms after reset; make sure we are reading.
        time.sleep(0.05)

    deadline = time.time() + args.seconds
    total = 0
    try:
        while time.time() < deadline:
            data = port.read(8192)
            if not data:
                continue
            total += len(data)
            text = data.decode("utf-8", errors="replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            if sink is not None:
                sink.write(text)
                sink.flush()
    except KeyboardInterrupt:
        pass
    finally:
        port.close()
        if sink is not None:
            sink.close()

    sys.stderr.write(f"\n[captured {total} bytes in {args.seconds:.0f}s]\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
