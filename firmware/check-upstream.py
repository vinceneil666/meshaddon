#!/usr/bin/env python3
"""Check the official MeshCore flasher for a new Heltec V4 repeater release.

Runs daily (systemd user timer meshaddon-check.timer, see firmware/). It:
  1. reads https://flasher.meshcore.io/config.json + /releases?repo=github (same source as the official flasher)
  2. writes fw/upstream.json: latest repeater version + files for each Heltec V4 variant
  3. if that version is newer than the newest MeshAddon build in fw/manifest.json and
     firmware/build-release.sh exists, runs it to build MeshAddon on top of the new tag

Usage: check-upstream.py [--no-build]
"""
import json, os, re, subprocess, sys, time, urllib.request

BASE = "https://flasher.meshcore.io"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "fw")
BUILDER = os.path.join(ROOT, "firmware", "build-release.sh")
UA = {"User-Agent": "meshaddon-check/0.1"}


def get(path):
    with urllib.request.urlopen(urllib.request.Request(BASE + path, headers=UA), timeout=30) as r:
        return json.load(r)


def vtuple(v):
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", v or "")
    return tuple(map(int, m.groups())) if m else (0, 0, 0)


def main():
    config, releases = get("/config.json"), get("/releases?repo=github")
    variants = {}
    for dev in config["device"]:
        if dev.get("maker") != "heltec" or "v4" not in dev["name"].lower():
            continue
        for fw in dev["firmware"]:
            src = fw.get("github")
            if fw.get("role") != "repeater" or not src:
                continue
            pats = {k: re.compile(v) for k, v in src["files"].items()}
            for rel in releases:                       # newest first
                if rel["type"] != src["type"]:
                    continue
                files = {k: next((f for f in rel["files"] if p.fullmatch(f["name"]) or p.match(f["name"])), None) for k, p in pats.items()}
                if files.get("flash-update"):
                    variants[dev["name"]] = {"version": rel["version"], "notes": rel.get("notes", ""),
                                             "files": {k: v for k, v in files.items() if v}}
                    break
    if not variants:
        sys.exit("no Heltec V4 repeater found in the flasher catalogue")

    latest = max((v["version"] for v in variants.values()), key=vtuple)
    out = {"checked": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "source": BASE, "latest": latest, "variants": variants}
    os.makedirs(FW, exist_ok=True)
    tmp = os.path.join(FW, "upstream.json.tmp")
    with open(tmp, "w") as f:
        json.dump(out, f, indent=1)
    os.replace(tmp, os.path.join(FW, "upstream.json"))

    try:
        builds = json.load(open(os.path.join(FW, "manifest.json"))).get("builds", [])
    except (OSError, ValueError):
        builds = []
    built = max((b.get("base", "") for b in builds), key=vtuple, default="")
    print(f"official Heltec V4 repeater: {latest}; newest MeshAddon build based on: {built or 'none'}")

    if vtuple(latest) > vtuple(built):
        if "--no-build" in sys.argv:
            print("new upstream release; build skipped (--no-build)")
        elif os.access(BUILDER, os.X_OK):
            print(f"building MeshAddon on {latest} …")
            subprocess.run([BUILDER, latest], check=True)
        else:
            print("new upstream release, but firmware/build-release.sh doesn't exist yet (phase 2)")


if __name__ == "__main__":
    main()
