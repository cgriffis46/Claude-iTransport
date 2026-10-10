#!/usr/bin/env python3
"""Tests for saleae_log.py on made-up exports (no real capture yet).

    python3 tools/saleae/test_saleae_log.py
"""

import csv
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import saleae_log  # noqa: E402

BAUD = 921600
BYTE_S = 10 / BAUD  # 8N1


def serial_rows(text, t0=0.001, style="hex"):
    rows = []
    t = t0
    for ch in text.encode("ascii"):
        if style == "hex":
            v = f"0x{ch:02X}"
        else:
            v = {13: "\\r", 10: "\\n", 32: "' '"}.get(ch, chr(ch))
        rows.append((t, v))
        t += BYTE_S
    return rows, t


def write_csv(path, header, rows):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


class SaleaeLogTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.dir.cleanup()

    def path(self, name):
        return os.path.join(self.dir.name, name)

    def test_logic2_hex(self):
        rows, _ = serial_rows("1 100 mtk3339 st 0 1\r\n2 101 mtk3339 cfg-send 314 0 0\r\n")
        write_csv(self.path("s.csv"), ["name", "type", "start_time", "duration", "data"],
                  [("Async Serial", "data", f"{t:.9f}", f"{BYTE_S:.9f}", v) for t, v in rows])
        out = saleae_log.timeline(saleae_log.read_serial(self.path("s.csv")))
        self.assertIn("1 100 mtk3339 st 0 1", out[0])
        self.assertIn("2 101 mtk3339 cfg-send 314 0 0", out[1])
        self.assertTrue(out[0].strip().startswith("0.001000"))
        self.assertEqual(out[-1], "-- 2 lines, 0 dropped on the board")

    def test_logic1_ascii_with_errors_and_gap(self):
        rows, _ = serial_rows("7 5 bus start 0\r\n9 6 bus xfer-fail 0\r\n", style="ascii")
        data = [(f"{t:.9f}", v, "", "") for t, v in rows]
        data[3] = (data[3][0], data[3][1], "", "Error")   # a framing error in the first line
        write_csv(self.path("s.csv"), ["Time [s]", "Value", "Parity Error", "Framing Error"], data)
        out = saleae_log.timeline(saleae_log.read_serial(self.path("s.csv")))
        text = "\n".join(out)
        self.assertIn("7 5 bus start 0  (UART errors: Error)", text)
        self.assertIn("!! 1 line(s) dropped on the board (seq 8..8)", text)
        self.assertIn("9 6 bus xfer-fail 0", text)
        self.assertEqual(out[-1], "-- 2 lines, 1 dropped on the board")

    def test_digital_merge_and_faults(self):
        rows, t_end = serial_rows("1 0 ssm st 0 1\r\n2 0 ssm fail 1\r\n")
        write_csv(self.path("s.csv"), ["start_time", "data"], [(f"{t:.9f}", v) for t, v in rows])
        write_csv(self.path("d.csv"), ["Time [s]", "Channel 0", "Channel 3"],
                  [("0.000000", "0", "0"), ("0.000500", "1", "0"), ("0.000600", "0", "0"),
                   (f"{t_end + 0.001:.6f}", "0", "1")])
        digital = saleae_log.read_digital(self.path("d.csv"))
        self.assertEqual(len(digital), 3)   # the first row is the start, not an edge
        pins = saleae_log.pin_names("0=bus-irq,3=fault", digital)
        out = saleae_log.timeline(saleae_log.read_serial(self.path("s.csv")), digital, pins)
        self.assertIn("pin bus-irq rise", out[0])   # 0.5 ms, before the first byte
        self.assertIn("pin bus-irq fall", out[1])
        self.assertIn("pin fault rise", out[-2])
        faults = saleae_log.timeline(saleae_log.read_serial(self.path("s.csv")), digital, pins, True)
        self.assertEqual(len(faults), 2)
        self.assertIn("2 0 ssm fail 1", faults[0])

    def test_restart_and_unfinished(self):
        rows, _ = serial_rows("5 9 a b\r\n1 0 a b\r\n2 0 a")
        write_csv(self.path("s.csv"), ["start_time", "data"], [(f"{t:.9f}", v) for t, v in rows])
        out = "\n".join(saleae_log.timeline(saleae_log.read_serial(self.path("s.csv"))))
        self.assertIn("seq went back from 5 to 1", out)
        self.assertIn("2 0 a  (unfinished)", out)

    def test_parse_byte(self):
        self.assertEqual(saleae_log.parse_byte("0x41"), 0x41)
        self.assertEqual(saleae_log.parse_byte("'A'"), 0x41)
        self.assertEqual(saleae_log.parse_byte("A"), 0x41)
        self.assertEqual(saleae_log.parse_byte("\\n"), 10)
        self.assertEqual(saleae_log.parse_byte("' '"), 32)
        self.assertIsNone(saleae_log.parse_byte(""))
        self.assertIsNone(saleae_log.parse_byte("framing"))


if __name__ == "__main__":
    unittest.main()
