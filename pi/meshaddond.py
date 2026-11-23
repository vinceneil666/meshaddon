#!/usr/bin/env python3
"""meshaddond - Raspberry Pi side of MeshAddon: run scripts from MeshCore.

The Heltec V4 (MeshAddon repeater build) answers all sensor/status commands itself.
The Pi link is ONLY used to list and run scripts in one folder:

    /opt/meshaddon/scripts/        (default, override with --scripts)
      uptime                       any executable file; name = [A-Za-z0-9_-]{1,24}
      wake-pc.sh                   extension is dropped from the mesh name: "wake-pc"
      ...

A script's description is the first line that starts with "# desc:".
Mesh usage (on the command channel, prefix "!"):
    !scripts            list (answered by the Heltec from its cached list)
    !run uptime         run the script; its stdout (first 140 chars) is the channel reply
    !run wake-pc office args are passed as argv, never through a shell

Environment given to scripts: MESH_FROM, MESH_CHANNEL, MESH_NODE, MESH_S<n> (sensor slot values).
Protocol: docs/PROTOCOL.md section B (newline-delimited JSON over UART).
"""
import argparse, json, os, re, socket, subprocess, sys, threading, time
import serial  # apt: python3-serial

VERSION = "0.2"
MAX_REPLY = 140
NAME_RE = re.compile(r"^[A-Za-z0-9_-]{1,24}$")
ARG_RE = re.compile(r"^[A-Za-z0-9_.,:=@+/-]{1,48}$")


def scan_scripts(folder):
    """Return {mesh_name: {"path":..., "desc":...}} for executable files in folder."""
    out = {}
    try:
        entries = sorted(os.scandir(folder), key=lambda e: e.name)
    except FileNotFoundError:
        return out
    for e in entries:
        if not e.is_file() or e.name.startswith("."):
            continue
        if not os.access(e.path, os.X_OK):
            continue
        name = os.path.splitext(e.name)[0]
        if not NAME_RE.match(name) or name in out:
            continue
        desc = ""
        try:
            with open(e.path, "r", errors="replace") as f:
                for _ in range(10):
                    line = f.readline()
                    if line.lower().startswith("# desc:"):
                        desc = line.split(":", 1)[1].strip()[:60]
                        break
        except OSError:
            pass
        out[name] = {"path": e.path, "desc": desc}
    return out


class Daemon:
    def __init__(self, a):
        self.a = a
        self.ser = serial.Serial(a.port, a.baud, timeout=1)
        self.lock = threading.Lock()
        self.scripts = {}
        self.sig = None
        self.running = set()
        self.node = "?"
        self.sensors = {}

    def send(self, obj):
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        with self.lock:
            self.ser.write(line.encode())

    def publish_scripts(self, force=False):
        s = scan_scripts(self.a.scripts)
        sig = tuple((k, v["desc"], os.stat(v["path"]).st_mtime) for k, v in s.items())
        if force or sig != self.sig:
            self.scripts, self.sig = s, sig
            self.send({"t": "scripts", "list": [{"name": k, "desc": v["desc"]} for k, v in s.items()]})
            print("scripts:", ", ".join(s) or "(none)", flush=True)

    def hello(self):
        self.send({"t": "hello", "host": socket.gethostname(), "ver": VERSION, "dir": self.a.scripts})
        self.publish_scripts(force=True)

    def run(self, msg):
        rid, name = msg.get("id"), str(msg.get("script", ""))
        args = [str(x) for x in msg.get("args", [])][:8]
        reply = lambda text: self.send({"t": "reply", "id": rid, "script": name, "text": text[:MAX_REPLY] or "ok"})
        self.publish_scripts()
        sc = self.scripts.get(name)
        if not sc:
            return reply(f"no script '{name}'. Try !scripts")
        bad = [x for x in args if not ARG_RE.match(x)]
        if bad:
            return reply(f"bad argument: {bad[0][:20]}")
        if name in self.running:
            return reply(f"{name} is already running")
        self.running.add(name)
        env = dict(os.environ, MESH_FROM=msg.get("from", ""), MESH_CHANNEL=msg.get("ch", ""), MESH_NODE=self.node)
        env.update({f"MESH_S{k}": str(v) for k, v in self.sensors.items()})
        t0 = time.monotonic()
        try:
            p = subprocess.run([sc["path"], *args], capture_output=True, text=True, timeout=self.a.timeout,
                               env=env, cwd=self.a.scripts, stdin=subprocess.DEVNULL)
            text = (p.stdout.strip() or p.stderr.strip() or f"exit {p.returncode}").replace("\n", " | ")
            if p.returncode != 0 and not text.startswith("exit"):
                text = f"[exit {p.returncode}] {text}"
        except subprocess.TimeoutExpired:
            text = f"{name}: timeout after {self.a.timeout}s"
        except OSError as e:
            text = f"{name}: {e.strerror}"
        finally:
            self.running.discard(name)
        print(f"run {name} {args} by {msg.get('from')} ({time.monotonic()-t0:.1f}s): {text[:80]}", flush=True)
        reply(text)

    def lines(self):
        buf = b""
        while True:
            chunk = self.ser.read(256)
            if chunk:
                buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                raw = raw.strip()
                if raw:
                    try:
                        yield json.loads(raw)
                    except ValueError:
                        print("bad line:", raw[:80], file=sys.stderr)
            yield None  # idle tick

    def main(self):
        os.makedirs(self.a.scripts, exist_ok=True)
        self.hello()
        last_ping = last_scan = time.monotonic()
        for msg in self.lines():
            now = time.monotonic()
            if now - last_ping > 10:
                self.send({"t": "pong"}); last_ping = now
            if now - last_scan > 15:
                self.publish_scripts(); last_scan = now
            if not msg:
                continue
            t = msg.get("t")
            if t == "hello":
                self.node = msg.get("node", "?")
                print("heltec:", msg, flush=True)
                self.hello()
            elif t == "ping":
                self.send({"t": "pong"})
            elif t == "list":
                self.publish_scripts(force=True)
            elif t == "sensors":
                self.sensors = msg.get("v", {})
            elif t == "run":
                threading.Thread(target=self.run, args=(msg,), daemon=True).start()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/serial0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--scripts", default="/opt/meshaddon/scripts")
    ap.add_argument("--timeout", type=int, default=30)
    Daemon(ap.parse_args()).main()
