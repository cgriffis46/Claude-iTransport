#!/usr/bin/env python3
"""PC side of the iNetTransport bring-up: exercises the board's test services.

The bring-up firmware (iNetTransport/examples/stm32l432kc_bringup) serves
  echo    TCP 7   everything sent comes back
  discard TCP 9   everything sent is dropped
  chargen TCP 19  a known pattern, until we close
  time    TCP 37  the board's clock, set by NTP (RFC 868)
on the W5500 interface, and echo alone on the ESP-AT one. This script
checks each byte that comes back, and measures what the link does:

  python3 net_bringup.py 192.168.1.77            # Ethernet: everything
  python3 net_bringup.py 192.168.4.20 --wifi     # ESP-AT: echo tests only
  python3 net_bringup.py --self-test             # the script against a local stand-in

Exit status 0 when every test passed. Standard library only.
"""

import argparse
import os
import random
import struct
import socket
import statistics
import sys
import threading
import time

ECHO, DISCARD, CHARGEN, TIME = 7, 9, 19, 37
RFC868_TO_UNIX = 2208988800


def chargen_byte(offset):
    """Same stream as chargen_byte() in the firmware's services.cpp."""
    line, col = divmod(offset, 74)
    if col == 72:
        return 13
    if col == 73:
        return 10
    return 32 + (line + col) % 95


def chargen_bytes(start, n):
    return bytes(chargen_byte(start + i) for i in range(n))


class Results:
    def __init__(self):
        self.rows = []

    def add(self, name, ok, detail):
        self.rows.append((name, ok, detail))
        print(f"  {'ok  ' if ok else 'FAIL'}  {name}: {detail}", flush=True)

    @property
    def failed(self):
        return [r for r in self.rows if not r[1]]


def connect(host, port, timeout):
    s = socket.create_connection((host, port), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def recv_exact(s, n):
    out = bytearray()
    while len(out) < n:
        chunk = s.recv(min(65536, n - len(out)))
        if not chunk:
            raise ConnectionError(f"closed after {len(out)} of {n} bytes")
        out += chunk
    return bytes(out)


def test_connect(r, host, port, timeout, n=10):
    times = []
    try:
        for _ in range(n):
            t0 = time.perf_counter()
            s = connect(host, port, timeout)
            times.append((time.perf_counter() - t0) * 1000)
            s.close()
            time.sleep(0.05)
        r.add("connect", True, f"{n} handshakes, {statistics.median(times):.1f} ms median, {max(times):.1f} ms worst")
    except OSError as e:
        r.add("connect", False, f"{e} (address right? board on the same network? services started?)")
        return False
    return True


def test_echo_sizes(r, host, timeout):
    sizes = [1, 2, 10, 100, 255, 256, 257, 1000, 1460, 2048, 4000, 8192, 16384]
    try:
        with connect(host, ECHO, timeout) as s:
            for n in sizes:
                data = os.urandom(n)
                s.sendall(data)
                back = recv_exact(s, n)
                if back != data:
                    bad = next(i for i in range(n) if back[i] != data[i])
                    r.add("echo sizes", False, f"{n}-byte message came back different from byte {bad}")
                    return
        r.add("echo sizes", True, f"{len(sizes)} messages, 1 B to 16 KB, every byte right")
    except (OSError, ConnectionError) as e:
        r.add("echo sizes", False, str(e))


def test_latency(r, host, timeout, n=100):
    rtts = []
    try:
        with connect(host, ECHO, timeout) as s:
            for i in range(n):
                msg = i.to_bytes(4, "big") * 8
                t0 = time.perf_counter()
                s.sendall(msg)
                if recv_exact(s, len(msg)) != msg:
                    r.add("latency", False, f"round trip {i} came back wrong")
                    return
                rtts.append((time.perf_counter() - t0) * 1000)
        rtts.sort()
        r.add("latency", True, f"{n} x 32 B round trips: min {rtts[0]:.2f}, median {statistics.median(rtts):.2f}, "
                               f"p95 {rtts[int(n * 0.95) - 1]:.2f}, max {rtts[-1]:.2f} ms")
    except (OSError, ConnectionError) as e:
        r.add("latency", False, str(e))


def test_upload(r, host, timeout, total):
    try:
        with connect(host, DISCARD, timeout) as s:
            block = os.urandom(4096)
            t0 = time.perf_counter()
            sent = 0
            while sent < total:
                s.sendall(block)
                sent += len(block)
            s.shutdown(socket.SHUT_WR)
            s.settimeout(timeout)
            try:
                s.recv(1)  # until the board closes too: everything has arrived
            except OSError:
                pass
            dt = time.perf_counter() - t0
        r.add("upload (discard)", True, f"{sent / 1024:.0f} KB in {dt:.2f} s: {sent / dt / 1024:.0f} KB/s")
    except OSError as e:
        r.add("upload (discard)", False, str(e))


def test_download(r, host, timeout, total):
    try:
        with connect(host, CHARGEN, timeout) as s:
            got = 0
            t0 = time.perf_counter()
            while got < total:
                chunk = s.recv(65536)
                if not chunk:
                    raise ConnectionError(f"closed after {got} bytes")
                if chunk != chargen_bytes(got, len(chunk)):
                    exp = chargen_bytes(got, len(chunk))
                    bad = next(i for i in range(len(chunk)) if chunk[i] != exp[i])
                    r.add("download (chargen)", False, f"byte {got + bad} wrong")
                    return
                got += len(chunk)
            dt = time.perf_counter() - t0
        r.add("download (chargen)", True, f"{got / 1024:.0f} KB in {dt:.2f} s, pattern checked: {got / dt / 1024:.0f} KB/s")
    except (OSError, ConnectionError) as e:
        r.add("download (chargen)", False, str(e))


def test_time(r, host, timeout, tolerance):
    """The board's clock against this PC's (both should be NTP-synced)."""
    try:
        with connect(host, TIME, timeout) as s:
            data = b""
            while len(data) < 4:
                chunk = s.recv(4 - len(data))
                if not chunk:
                    break
                data += chunk
        if len(data) < 4:
            r.add("time (NTP)", False, "the board has no time yet: check its log for the NTP step")
            return
        board = struct.unpack(">I", data)[0] - RFC868_TO_UNIX
        diff = board - time.time()
        ok = abs(diff) <= tolerance
        stamp = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(board))
        r.add("time (NTP)", ok, f"board says {stamp}, {diff:+.1f} s from this PC"
              + ("" if ok else f" (more than {tolerance} s: is this PC's clock right?)"))
    except OSError as e:
        r.add("time (NTP)", False, str(e))


def test_churn(r, host, timeout, n):
    try:
        for i in range(n):
            with connect(host, ECHO, timeout) as s:
                msg = f"churn {i}\n".encode()
                s.sendall(msg)
                if recv_exact(s, len(msg)) != msg:
                    r.add("connection churn", False, f"connection {i}: wrong echo")
                    return
        r.add("connection churn", True, f"{n} connect / echo / close cycles")
    except (OSError, ConnectionError) as e:
        r.add("connection churn", False, f"{e}: sockets not given back on close?")


def test_concurrent(r, host, timeout, total):
    errors = []

    def run(fn, *a):
        sub = Results()
        sub.add = lambda name, ok, detail: (None if ok else errors.append(f"{name}: {detail}"))
        fn(sub, *a)

    threads = [threading.Thread(target=run, args=(test_upload, host, timeout, total)),
               threading.Thread(target=run, args=(test_download, host, timeout, total)),
               threading.Thread(target=run, args=(test_echo_sizes, host, timeout))]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    dt = time.perf_counter() - t0
    if errors:
        r.add("all three at once", False, "; ".join(errors))
    else:
        r.add("all three at once", True, f"upload, download and echo together, {dt:.2f} s")


# --- a stand-in for the board, to check this script ------------------------

def serve(port, handler, stop):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(8)
    ls.settimeout(0.2)
    while not stop.is_set():
        try:
            c, _ = ls.accept()
        except socket.timeout:
            continue
        threading.Thread(target=handler, args=(c,), daemon=True).start()
    ls.close()


def h_echo(c):
    with c:
        while (d := c.recv(4096)):
            c.sendall(d)


def h_discard(c):
    with c:
        while c.recv(4096):
            pass


def h_time(c):
    with c:
        c.sendall(struct.pack(">I", int(time.time()) + RFC868_TO_UNIX))


def h_chargen(c):
    with c:
        off = 0
        try:
            while True:
                c.sendall(chargen_bytes(off, 4096))
                off += 4096
        except OSError:
            pass


def self_test(args):
    global ECHO, DISCARD, CHARGEN, TIME
    base = random.randint(20000, 40000)
    ECHO, DISCARD, CHARGEN, TIME = base, base + 1, base + 2, base + 3
    stop = threading.Event()
    for port, h in ((ECHO, h_echo), (DISCARD, h_discard), (CHARGEN, h_chargen), (TIME, h_time)):
        threading.Thread(target=serve, args=(port, h, stop), daemon=True).start()
    time.sleep(0.3)
    print(f"self-test against a local stand-in on ports {ECHO}, {DISCARD}, {CHARGEN}, {TIME}")
    args.host = "127.0.0.1"
    try:
        return run(args)
    finally:
        stop.set()


def run(args):
    r = Results()
    print(f"bring-up tests against {args.host}{' (Wi-Fi: echo only)' if args.wifi else ''}")
    if not test_connect(r, args.host, ECHO, args.timeout):
        return r
    test_echo_sizes(r, args.host, args.timeout)
    test_latency(r, args.host, args.timeout)
    test_churn(r, args.host, args.timeout, args.churn)
    if not args.wifi:
        total = args.kb * 1024
        test_upload(r, args.host, args.timeout, total)
        test_download(r, args.host, args.timeout, total)
        test_concurrent(r, args.host, args.timeout, total // 4)
        test_time(r, args.host, args.timeout, args.time_tolerance)
    return r


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host", nargs="?", help="the board's address (from its log)")
    p.add_argument("--wifi", action="store_true", help="ESP-AT interface: echo tests only")
    p.add_argument("--kb", type=int, default=1024, help="KB for each throughput test (default 1024)")
    p.add_argument("--churn", type=int, default=50, help="connect/close cycles (default 50)")
    p.add_argument("--timeout", type=float, default=10.0, help="seconds before a stalled step fails (default 10)")
    p.add_argument("--time-tolerance", type=float, default=2.0,
                   help="seconds the board's clock may differ from this PC's (default 2)")
    p.add_argument("--self-test", action="store_true", help="run against a local stand-in, to check the script")
    args = p.parse_args()
    if not args.self_test and not args.host:
        p.error("give the board's address, or --self-test")

    r = self_test(args) if args.self_test else run(args)
    print()
    if r.failed:
        print(f"FAILED: {len(r.failed)} of {len(r.rows)} tests")
        return 1
    print(f"PASSED: all {len(r.rows)} tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
