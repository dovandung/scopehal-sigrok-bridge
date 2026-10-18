#!/usr/bin/env python3
"""
Minimal stand-in for ngscopeclient's DSLabs driver, for testing the bridge
without a GUI. Sends the same SCPI commands and parses the binary waveform
stream exactly as DSLabsOscilloscope::AcquireData() does.

  python3 tools/test_client.py [host] [--count N] [--rate HZ] [--depth N]
"""
import argparse
import socket
import struct
import sys
import time


class Bridge:
    def __init__(self, host, port):
        self.ctl = socket.create_connection((host, port))
        self.ctl_f = self.ctl.makefile("rwb")
        self.data = socket.create_connection((host, port + 1))
        self.data.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def cmd(self, s):
        self.ctl_f.write((s + "\n").encode())
        self.ctl_f.flush()

    def query(self, s):
        self.cmd(s)
        return self.ctl_f.readline().decode().strip()

    def recv(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.data.recv(n - len(buf))
            if not chunk:
                raise EOFError("data socket closed")
            buf += chunk
        return bytes(buf)

    def acquire(self, analog_count):
        """One waveform, parsed like DSLabsOscilloscope::AcquireData()."""
        self.data.sendall(b"K")
        seq, nch = struct.unpack("<IH", self.recv(6))
        fs_per_sample, trigger_fs = struct.unpack("<qq", self.recv(16))
        (wfms_s,) = struct.unpack("<d", self.recv(8))
        chans = {}
        for _ in range(nch):
            chnum, memdepth = struct.unpack("<QQ", self.recv(16))
            if chnum < analog_count:
                scale, offset, trigphase = struct.unpack("<fff", self.recv(12))
                (clip,) = struct.unpack("<?", self.recv(1))
                raw = self.recv(memdepth)
                chans[chnum] = ("analog", [b * scale - offset for b in raw])
            else:
                (first_sample,) = struct.unpack("<i", self.recv(4))
                raw = self.recv(memdepth)
                bits = [(raw[m] >> b) & 1 for m in range(memdepth) for b in range(8)]
                chans[chnum] = ("digital", first_sample, bits)
        return dict(seq=seq, fs_per_sample=fs_per_sample, trigger_fs=trigger_fs,
                    wfms_s=wfms_s, chans=chans)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="localhost")
    ap.add_argument("--port", type=int, default=5025)
    ap.add_argument("--count", type=int, default=5)
    ap.add_argument("--rate", type=int)
    ap.add_argument("--depth", type=int, default=10000)
    args = ap.parse_args()

    b = Bridge(args.host, args.port)
    print("*IDN?     ", b.query("*IDN?"))
    total = int(b.query("CHANS?"))
    ana = int(b.query("ANACHANS?"))
    dig = int(b.query("DIGCHANS?"))
    print("CHANS?    ", total, " ANACHANS?", ana, " DIGCHANS?", dig)
    rates = [float(x) for x in b.query("RATES?").split(",") if x]
    print("RATES?    ", len(rates), "rates, fastest", f"{1e15 / min(rates):.0f} Hz" if rates else "-")
    print("DEPTHS?   ", b.query("DEPTHS?"))

    rate = args.rate or (int(1e15 / min(rates)) if rates else 1000000)
    b.cmd(f"RATE {rate}")
    b.cmd(f"DEPTH {args.depth}")
    b.cmd("TRIG:SOU 0")
    b.cmd("TRIG:EDGE:DIR RISING")
    b.cmd(f"TRIG:DELAY {int(1e15 / rate * args.depth / 2)}")  # trigger mid-capture
    print("ARMED?    ", b.query("ARMED?"))
    b.cmd("START")
    print("ARMED?    ", b.query("ARMED?"), "(after START)")

    t0 = time.time()
    for i in range(args.count):
        w = b.acquire(ana)
        summary = []
        for ch, v in sorted(w["chans"].items()):
            if v[0] == "digital":
                bits = v[2]
                edges = sum(1 for k in range(1, len(bits)) if bits[k] != bits[k - 1])
                summary.append(f"ch{ch}:{len(bits)}smp/{edges}edges/first={v[1]}")
            else:
                summary.append(f"ch{ch}:{len(v[1])}smp")
        print(f"wfm seq={w['seq']} {1e15 / w['fs_per_sample']:.0f}Hz trig={w['trigger_fs']}fs "
              f"hw={w['wfms_s']:.1f}/s  " + " ".join(summary[:4]) + (" ..." if len(summary) > 4 else ""))
    dt = time.time() - t0
    print(f"{args.count} waveforms in {dt:.2f}s")

    b.cmd("STOP")
    print("ARMED?    ", b.query("ARMED?"), "(after STOP)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
