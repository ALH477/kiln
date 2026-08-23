#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# tools/n64-drive.sh — boot a ROM in Ares with a virtual controller and
# run an input script against it, capturing screenshots along the way.
#
# Usage: n64-drive.sh <rom.z64> <script.txt> [outdir] [settle-seconds]
#
# This is tools/n64-shot.sh with two additions: a working controller, and
# a script instead of a single grab. Everything the shot script learned the
# hard way applies here unchanged and is repeated below rather than
# referred to, because a capture that silently grabs the wrong thing is
# the failure this whole path exists to rule out:
#
#  * The ROM is copied somewhere WRITABLE first (Ares pops a read-only-save
#    modal on a Nix store path and then sits in it).
#  * --settings-file points at a throwaway config, so a run never mutates
#    the user's real Ares settings. Here it is not merely throwaway — it is
#    GENERATED, because a fresh ares config has every input binding empty.
#  * The window is matched by the PID we launched, never by a class or
#    title substring.
#  * Ares is moved onto the workspace the monitor is actually showing, and
#    that is asserted afterwards: grim captures a screen rectangle off the
#    composited output and has no concept of a window, so a mismatch yields
#    a perfectly plausible screenshot of something else entirely.
#
# ── The input half ────────────────────────────────────────────────────
# tools/n64-input.py creates a uinput gamepad, probes it through the same
# libSDL3 ares links to learn the GUID and index of every control, and
# writes the bindings into the generated settings file.
#
# It runs as ONE long-lived process (`serve`) that owns the pad for the
# whole session, and this script waits on it rather than the other way
# round. That is not tidiness: ares resolves each binding to a live device
# when its settings load, and a binding naming a device that is not
# present at that moment stays unbound for the rest of the run. A pad
# created after ares — or created, probed, closed, and recreated — produces
# a capture where every screen is reachable in principle and none is in
# practice, which looks exactly like a game that ignores input.
#
# Hence the handshake: serve writes $READY when the bindings are on disk
# and the pad is live, this script starts ares and finds its window, then
# writes the geometry to $START, which is serve's cue to begin.
set -euo pipefail

ROM="${1:?usage: n64-drive.sh <rom.z64> <script.txt> [outdir] [settle-seconds]}"
SCRIPT="${2:?usage: n64-drive.sh <rom.z64> <script.txt> [outdir] [settle-seconds]}"
OUTDIR="${3:-.}"
SETTLE="${4:-6}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for tool in hyprctl grim python3; do
  command -v "$tool" >/dev/null || { echo "n64-drive: '$tool' not found (needs Hyprland + grim)" >&2; exit 1; }
done
[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || { echo "n64-drive: not inside a Hyprland session" >&2; exit 1; }
[ -f "$SCRIPT" ] || { echo "n64-drive: no such input script: $SCRIPT" >&2; exit 1; }

ARES="${ARES:-ares}"
command -v "$ARES" >/dev/null || { echo "n64-drive: ares not on PATH" >&2; exit 1; }
export ARES

WORK="$(mktemp -d)"
cleanup() {
  [ -n "${PAD_PID:-}" ] && kill "$PAD_PID" 2>/dev/null || true
  [ -n "${EMU_PID:-}" ] && kill "$EMU_PID" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

cp "$ROM" "$WORK/rom.z64"
chmod u+w "$WORK/rom.z64"
mkdir -p "$OUTDIR"

# SDL ignores joystick activity from an unfocused application unless this
# is set, and a drive run cannot promise ares keeps focus: it dispatches
# hyprctl and shells out to grim while the ROM is running. Exported before
# BOTH processes, since each has its own SDL.
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1

# ── 1. The pad, and the bindings for it ───────────────────────────────
python3 "$HERE/n64-input.py" serve "$SCRIPT" \
        --bml "$WORK/settings.bml" \
        --ready "$WORK/ready" --start "$WORK/start" \
        --outdir "$OUTDIR" &
PAD_PID=$!

for _ in $(seq 1 100); do
  [ -f "$WORK/ready" ] && break
  kill -0 "$PAD_PID" 2>/dev/null || break
  sleep 0.2
done
if [ ! -f "$WORK/ready" ]; then
  echo "n64-drive: the virtual pad never came up — no input would reach the ROM" >&2
  exit 1
fi

"$ARES" --system "Nintendo 64" \
        --no-file-prompt --kiosk \
        --settings-file "$WORK/settings.bml" \
        "$WORK/rom.z64" >"$WORK/ares.log" 2>&1 &
EMU_PID=$!

find_ares_geom() {
  hyprctl clients -j 2>/dev/null | EMU_PID="$EMU_PID" python3 -c '
import json, os, sys
try:
    clients = json.load(sys.stdin)
except Exception:
    sys.exit()
pid = int(os.environ["EMU_PID"])
def alive(c):
    return c.get("mapped") and not c.get("hidden")
match = None
for c in clients:
    if c.get("pid") == pid and alive(c):
        match = c
        break
if match is None:
    for c in clients:
        if (c.get("class", "").lower() == "ares" or c.get("initialClass", "").lower() == "ares") and alive(c):
            match = c
            break
if match:
    print("%d,%d %dx%d" % (match["at"][0], match["at"][1], match["size"][0], match["size"][1]))
' 2>/dev/null || true
}
ares_ws() {
  hyprctl clients -j 2>/dev/null | EMU_PID="$EMU_PID" python3 -c '
import json, os, sys
try: clients = json.load(sys.stdin)
except Exception: sys.exit()
pid = int(os.environ["EMU_PID"])
for c in clients:
    if c.get("pid") == pid and c.get("mapped") and not c.get("hidden"):
        print(c["workspace"]["id"]); break
' 2>/dev/null || true
}
active_ws() {
  hyprctl monitors -j 2>/dev/null | python3 -c '
import json, sys
for m in json.load(sys.stdin):
    if m.get("focused"): print(m["activeWorkspace"]["id"]); break
' 2>/dev/null || true
}

GEOM=""
for _ in $(seq 1 60); do
  sleep 0.5
  GEOM="$(find_ares_geom)"
  [ -n "$GEOM" ] && break
done
if [ -z "$GEOM" ]; then
  echo "n64-drive: ares window never appeared" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi

AWS="$(active_ws)"; EWS="$(ares_ws)"
if [ -n "$AWS" ] && [ -n "$EWS" ] && [ "$AWS" != "$EWS" ]; then
  echo "n64-drive: ares is on workspace $EWS, screen is showing $AWS — moving it"
  hyprctl dispatch movetoworkspacesilent "$AWS,pid:$EMU_PID" >/dev/null 2>&1 || true
  hyprctl dispatch focuswindow "pid:$EMU_PID" >/dev/null 2>&1 || true
  sleep 1
  GEOM="$(find_ares_geom)"
fi

sleep "$SETTLE"

AWS="$(active_ws)"; EWS="$(ares_ws)"
if [ -n "$AWS" ] && [ -n "$EWS" ] && [ "$AWS" != "$EWS" ]; then
  echo "n64-drive: ares is on workspace $EWS but the screen shows $AWS." >&2
  echo "n64-drive: every shot would capture the wrong workspace — refusing." >&2
  exit 1
fi

GEOM2="$(find_ares_geom)"
if [ -z "$GEOM2" ]; then
  echo "n64-drive: ares window closed during the settle period — nothing to drive" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi
GEOM="$GEOM2"

echo "n64-drive: driving $(basename "$ROM") with $(basename "$SCRIPT") (window $GEOM)"

# ── 2. The run ────────────────────────────────────────────────────────
# Hand the geometry over; that is serve's cue to start typing.
printf '%s\n' "$GEOM" > "$WORK/start"
set +e
wait "$PAD_PID"
RC=$?
set -e

# Ares dying mid-script turns every subsequent shot into a capture of
# whatever the compositor reflowed into that rectangle, which is exactly
# the silently-wrong-PNG case. Report it rather than letting the numbers
# speak for a window that was not there.
if ! kill -0 "$EMU_PID" 2>/dev/null; then
  echo "n64-drive: ares exited before the script finished — later shots are suspect" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi

echo "n64-drive: done (shots in $OUTDIR)"
exit $RC
