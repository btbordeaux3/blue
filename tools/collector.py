#!/usr/bin/env python3
"""Read Wi-Fi scan lines from the ESP32 over USB and save labelled CSV files.

Usage:
    python tools/collector.py office
    python tools/collector.py bedroom --port /dev/ttyUSB0
"""

import argparse
import csv
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


def open_port(preferred, baud):
    candidates = [preferred] if preferred else []
    if not candidates:
        found = sorted(Path("/dev").glob("ttyUSB*")) + sorted(Path("/dev").glob("ttyACM*"))
        if not found:
            sys.exit("No serial ports found under /dev. Pass --port explicitly.")
        candidates = [str(p) for p in found]
    for cand in candidates:
        try:
            port = serial.Serial(cand, baud, timeout=1)
            print(f"Opened {cand}")
            return port
        except serial.SerialException as exc:
            print(f"Skipping {cand}: {exc}")
    sys.exit("Could not open any serial port.")


def parse_line(line):
    parts = [p.strip() for p in line.split(",")]
    if len(parts) != 5:
        return None
    ts_s, bssid, ssid, rssi_s, chan_s = parts
    if bssid.count(":") != 5:
        return None
    try:
        ts = int(float(ts_s))
        rssi = int(rssi_s)
        channel = int(chan_s)
    except ValueError:
        return None
    if not (-128 <= rssi <= 0) or not (1 <= channel <= 196):
        return None
    return ts, bssid, ssid, rssi, channel


def main():
    ap = argparse.ArgumentParser(description="Collect ESP32 Wi-Fi scans into CSV")
    ap.add_argument("room", help="Room label, e.g. office or bedroom")
    ap.add_argument("--port", help="Serial port (auto-detected if omitted)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--outdir", default="data")
    args = ap.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    port = open_port(args.port, args.baud)
    port.reset_input_buffer()

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    outpath = outdir / f"{args.room}_{stamp}.csv"
    print(f"Saving to {outpath}  (Ctrl+C to stop)")

    started = time.monotonic()
    rows = 0
    skipped = 0
    try:
        with outpath.open("w", newline="") as fh:
            writer = csv.writer(fh)
            writer.writerow(["timestamp", "room", "bssid", "ssid", "rssi", "channel"])
            fh.flush()
            while True:
                raw = port.readline()
                if not raw:
                    continue
                line = raw.decode("ascii", errors="replace").strip()
                if not line or line.startswith("timestamp"):
                    continue
                parsed = parse_line(line)
                if parsed is None:
                    skipped += 1
                    continue
                ts, bssid, ssid, rssi, channel = parsed
                writer.writerow([ts, args.room, bssid, ssid, rssi, channel])
                rows += 1
                if rows % 50 == 0:
                    fh.flush()
                    elapsed = time.monotonic() - started
                    rate = rows / elapsed if elapsed > 0 else 0.0
                    print(f"{rows} rows ({rate:.1f} rows/s, {skipped} skipped)")
    except KeyboardInterrupt:
        pass
    finally:
        port.close()

    print(f"Saved {rows} rows to {outpath}")


if __name__ == "__main__":
    main()
