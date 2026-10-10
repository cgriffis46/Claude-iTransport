#!/usr/bin/env python3
"""Turn Saleae Logic CSV exports back into a timeline of debug lines.

The firmware's debug log (iTransport/itransport/inc/DebugLog.h) sends
text lines on a UART:

    <seq> <ms> <tag> <what>[ <value>...]

Capture that UART with the Async Serial analyser and export its table
as CSV; optionally export the digital channels (the debug pins) as CSV
too. This script joins the bytes back into lines, stamps each with the
analyser's time of its first byte, finds gaps in seq (lines dropped on
the board), and merges in the pin edges, in time order:

    python3 saleae_log.py --serial serial.csv [--digital digital.csv]
        [--pins "0=bus-irq,1=transfer,2=state,3=fault"] [--faults]

Formats read (from Saleae's documentation as remembered, not checked
against a real export yet: send one and the parser will be fixed to it):
  - Logic 2 analyser table: a header with "start_time" (or "Time")
    and "data" (or "value") columns, one row per byte.
  - Logic 1 analyser export: "Time [s],Value,Parity Error,Framing Error".
  - Byte values as hex (0x41; the most reliable, set the analyser's
    display radix to hex before exporting) or as single characters,
    including the escapes \\r \\n \\t and quoted 'A'.
  - Digital export: "Time [s],<channel>,<channel>..." with 0/1 values,
    one row per change.
A row whose error column is not empty is reported as a framing error.
"""

import argparse
import csv
import re
import sys

# ---- reading the exports ----

_ESCAPES = {"\\r": "\r", "\\n": "\n", "\\t": "\t", "\\0": "\0", "' '": " ", "space": " ",
            "[cr]": "\r", "[lf]": "\n", "cr": "\r", "lf": "\n"}


def _find(header, *names):
    """Index of the first column whose name contains one of names."""
    low = [h.strip().lower() for h in header]
    for name in names:
        for i, h in enumerate(low):
            if h == name:
                return i
    for name in names:
        for i, h in enumerate(low):
            if name in h:
                return i
    return None


def parse_byte(text):
    """A byte from the export's data column, or None if it isn't one."""
    t = text.strip()
    if t == "":
        return None
    if t.lower().startswith("0x"):
        return int(t, 16) & 0xFF
    low = t.lower()
    if low in _ESCAPES:
        return ord(_ESCAPES[low])
    if len(t) >= 3 and t[0] == t[-1] and t[0] in "'\"":
        inner = t[1:-1]
        if inner.lower() in _ESCAPES:
            return ord(_ESCAPES[inner.lower()])
        if len(inner) == 1:
            return ord(inner)
    if len(t) == 1:
        return ord(t)
    return None


def read_serial(path):
    """[(time_s, byte, error_text)] from an analyser export."""
    out = []
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows:
        return out
    header = rows[0]
    ti = _find(header, "start_time", "time [s]", "time")
    di = _find(header, "data", "value")
    ei = [i for i, h in enumerate(header) if "error" in h.lower()]
    if ti is None or di is None:
        raise ValueError(f"{path}: no time or data column in header {header}")
    for r in rows[1:]:
        if len(r) <= max(ti, di):
            continue
        b = parse_byte(r[di])
        if b is None:
            continue
        err = ";".join(r[i].strip() for i in ei if i < len(r) and r[i].strip() not in ("", "0", "false", "False"))
        out.append((float(r[ti]), b, err))
    return out


def read_digital(path):
    """[(time_s, channel_name, level)] for every change, from a digital export."""
    out = []
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows:
        return out
    header = [h.strip() for h in rows[0]]
    ti = _find(header, "time [s]", "time")
    last = {}
    for r in rows[1:]:
        if len(r) < len(header):
            continue
        t = float(r[ti])
        for i, name in enumerate(header):
            if i == ti:
                continue
            v = r[i].strip()
            if v not in ("0", "1"):
                continue
            level = int(v)
            if last.get(name) != level:
                if name in last:   # the first row is the starting level, not an edge
                    out.append((t, name, level))
                last[name] = level
    return out


# ---- making lines ----

LINE = re.compile(r"^(\d+) (\d+) (\S+) (\S+)((?: -?\d+)*)$")


def lines_from_bytes(data):
    """[(time_s, text, errors)] split at \\n, \\r dropped."""
    out = []
    cur, t0, errs = [], None, []
    for t, b, err in data:
        if t0 is None:
            t0 = t
        if err:
            errs.append(err)
        if b == 0x0A:
            out.append((t0, bytes(cur).decode("ascii", "replace"), errs))
            cur, t0, errs = [], None, []
        elif b != 0x0D:
            cur.append(b)
    if cur:
        out.append((t0, bytes(cur).decode("ascii", "replace") + "  (unfinished)", errs))
    return out


def pin_names(spec, digital):
    """{channel column name: label} from --pins "0=bus-irq,1=transfer"."""
    names = {}
    if not spec:
        return names
    channels = sorted({c for _, c, _ in digital})
    for part in spec.split(","):
        if "=" not in part:
            continue
        k, label = part.split("=", 1)
        k = k.strip()
        for c in channels:
            if c == k or re.search(r"(?:^|\D)" + re.escape(k) + r"$", c):
                names[c] = label.strip()
    return names


def timeline(serial, digital=(), pins=None, faults_only=False):
    """The merged report, as a list of text lines."""
    pins = pins or {}
    events = []
    gaps = 0
    last_seq = None
    for t, text, errs in lines_from_bytes(serial):
        m = LINE.match(text)
        note = ""
        if m:
            seq = int(m.group(1))
            if last_seq is not None and seq != last_seq + 1:
                missing = seq - last_seq - 1
                if missing > 0:
                    gaps += missing
                    events.append((t, 0, f"!! {missing} line(s) dropped on the board (seq {last_seq + 1}..{seq - 1})"))
                else:
                    events.append((t, 0, f"!! seq went back from {last_seq} to {seq}: the board restarted?"))
            last_seq = seq
            if faults_only and m.group(4) not in FAULTS:
                continue
        elif text:
            note = "  (not a log line)"
        if errs:
            note += "  (UART errors: " + ",".join(errs) + ")"
        events.append((t, 1, text + note))
    if not faults_only:
        for t, ch, level in digital:
            events.append((t, 2, f"pin {pins.get(ch, ch)} {'rise' if level else 'fall'}"))
    events.sort(key=lambda e: (e[0], e[1]))
    out = [f"{t:12.6f}  {text}" for t, _, text in events]
    out.append(f"-- {sum(1 for e in events if e[1] == 1)} lines, {gaps} dropped on the board")
    return out


# The "what" of faults (DBG_FAULT) in this repository, for --faults.
FAULTS = {
    "fail", "issue-timeout", "bus-err", "land-timeout",
    "xfer-fail", "irq-idle", "irq-unknown", "no-slot",
    "cfg-nak", "cfg-reject", "cfg-noanswer", "cfg-uart-busy", "ubx-crc", "rx-overflow", "silent",
    "timeout", "crc", "own-id", "bad-flags", "new-partner", "seq",
}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--serial", required=True, help="the Async Serial analyser's CSV export")
    ap.add_argument("--digital", help="the digital channels' CSV export (the debug pins)")
    ap.add_argument("--pins", help='names for the channels, e.g. "0=bus-irq,1=transfer,2=state,3=fault"')
    ap.add_argument("--faults", action="store_true", help="only the faults (and dropped lines)")
    a = ap.parse_args(argv)
    serial = read_serial(a.serial)
    digital = read_digital(a.digital) if a.digital else []
    for line in timeline(serial, digital, pin_names(a.pins, digital), a.faults):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
