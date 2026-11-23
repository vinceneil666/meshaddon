#!/bin/bash
# Build MeshAddon on top of an official MeshCore repeater release and publish it to fw/.
#   firmware/build-release.sh v1.18.0            (called by check-upstream.py when a new release appears)
# Steps: fresh worktree at tag repeater-<ver> -> copy addon/ -> apply meshaddon.patch -> pio build + mergebin
#        -> fw/meshaddon-heltec_v4-<ver>-addon<addon>.bin (+ -merged.bin) -> fw/manifest.json
set -euo pipefail
VER="${1:?usage: build-release.sh vX.Y.Z}"
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(dirname "$HERE")"
ADDON_VER="$(sed -n 's/.*#define MESHADDON_VERSION "\(.*\)".*/\1/p' "$HERE/addon/MeshAddon.h" | head -1)"
ENV=heltec_v4_repeater_addon; BOARD="Heltec v4"; SLUG=heltec_v4
SRC="$HOME/MeshCore"; mkdir -p "$HERE/.build"; WT="$(mktemp -d "$HERE/.build/wt.XXXX")"
OUT="$ROOT/fw"; NAME="meshaddon-${SLUG}-${VER}-addon${ADDON_VER}"
export PATH="$PATH:$HOME/.platformio/penv/bin"
cleanup(){ git -C "$SRC" worktree remove --force "$WT" >/dev/null 2>&1 || true; }
trap cleanup EXIT

git -C "$SRC" fetch -q --tags origin
git -C "$SRC" worktree add -q --detach "$WT" "repeater-${VER}"
cp "$HERE"/addon/MeshAddon.{h,cpp} "$WT/examples/simple_repeater/"
if ! git -C "$WT" apply --3way "$HERE/meshaddon.patch"; then
  echo "ERROR: meshaddon.patch does not apply to repeater-${VER}; update the patch by hand" >&2; exit 2
fi
cd "$WT"
export PLATFORMIO_BUILD_FLAGS="-D FIRMWARE_VERSION='\"${VER}-addon${ADDON_VER}\"' -D FIRMWARE_BUILD_DATE='\"$(LC_ALL=C date +'%d %b %Y')\"' -fmacro-prefix-map=$HOME/.platformio/=pio/ -fmacro-prefix-map=$WT/="
pio run -e "$ENV" -t mergebin >"$HERE/.build/build.log" 2>&1 || { tail -30 "$HERE/.build/build.log"; exit 1; }
B=".pio/build/$ENV"
mkdir -p "$OUT"
cp "$B/firmware.bin" "$OUT/$NAME.bin"; cp "$B/firmware-merged.bin" "$OUT/$NAME-merged.bin"

# privacy scan: no local paths/usernames/credentials in shared binaries
# extra local patterns (e.g. WiFi names) can be given in MESHADDON_PRIVACY_PATTERNS="a|b|c"
PAT="/home/|${USER:-nobody}${MESHADDON_PRIVACY_PATTERNS:+|$MESHADDON_PRIVACY_PATTERNS}"
if strings "$OUT/$NAME.bin" | grep -qiE "$PAT"; then
  echo "ERROR: private strings found in binary" >&2; rm -f "$OUT/$NAME"*.bin; exit 3
fi

python3 - "$OUT" "$NAME" "$VER" "$ADDON_VER" "$BOARD" "$ENV" <<'PY'
import hashlib, json, os, sys, time
out, name, ver, addon, board, env = sys.argv[1:]
mf = os.path.join(out, "manifest.json")
try: m = json.load(open(mf))
except Exception: m = {}
builds = [b for b in m.get("builds", []) if not (b.get("board") == board and b.get("base") == ver and b.get("addon") == addon)]
sha = lambda f: hashlib.sha256(open(os.path.join(out, f), "rb").read()).hexdigest()
builds.append({"board": board, "env": env, "base": ver, "addon": addon, "built": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
               "files": {"update": f"{name}.bin", "full": f"{name}-merged.bin"},
               "sha256": {"update": sha(f"{name}.bin"), "full": sha(f"{name}-merged.bin")},
               "notes": f"MeshAddon {addon} on official MeshCore repeater {ver}"})
builds.sort(key=lambda b: (b["board"], b["base"], b["addon"]), reverse=True)
json.dump({"builds": builds}, open(mf + ".tmp", "w"), indent=1); os.replace(mf + ".tmp", mf)
print(f"published {name} ({len(builds)} build(s) in manifest)")
PY
