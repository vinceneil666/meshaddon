#!/usr/bin/env python3
"""Test helper: exposes a serial port to firmware/test-flash.mjs (Node) as a Web-Serial-like port.
stdin -> serial TX, serial RX -> stdout, fd 3 = control lines: {"dtr":bool,"rts":bool} | {"baud":n}"""
import json, os, sys, threading, serial
port, baud = sys.argv[1], int(sys.argv[2])
p = serial.Serial(); p.port = port; p.baudrate = baud; p.timeout = 0.05; p.dtr = False; p.rts = False
p.open()
out = sys.stdout.buffer
def rx():
    while True:
        try: d = p.read(4096)
        except Exception: os._exit(0)
        if d: out.write(d); out.flush()
def ctrl():
    for line in os.fdopen(3, "r"):
        try: m = json.loads(line)
        except ValueError: continue
        if "dtr" in m and m["dtr"] is not None: p.dtr = bool(m["dtr"])
        if "rts" in m and m["rts"] is not None: p.rts = bool(m["rts"])
        if "baud" in m: p.baudrate = int(m["baud"])
threading.Thread(target=rx, daemon=True).start()
threading.Thread(target=ctrl, daemon=True).start()
while True:
    d = sys.stdin.buffer.read1(4096)
    if not d: break
    p.write(d)
p.close()
