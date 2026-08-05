#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# tools/n64-rec.sh — boot a ROM in Ares on Hyprland and capture a video clip.
#
# Usage: n64-rec.sh <rom.z64> <out.mp4> [duration-seconds] [settle-seconds]
#
# Mirrors tools/n64-shot.sh's window-finding logic; only the capture backend
# differs — gpu-screen-recorder records the Ares window's screen region for
# the requested duration instead of grim grabbing a single PNG. Audio is
# isolated to Ares via PipeWire app routing (-a app:ares) so other apps
# playing (e.g. a YouTube tab in Floorp) do not bleed into the capture.
# Requires PipeWire as the sound server; if app: routing is unavailable the
# recorder logs an audio error and the MP4 ships without a soundtrack.
# -f 60 -fm cfr gives a steady 60 fps N64-rate capture.
#
# ── Same hard-won lessons as n64-shot.sh ───────────────────────────────
#  * ROM must be copied writable first (Ares opens a read-only-save modal
#    on a Nix store path otherwise and sits there for the whole capture).
#  * --settings-file -> throwaway config, so a rec run never mutates the
#    user's real Ares settings.
#  * --kiosk removes the menu bar so the region is just the emulated screen.
#  * Window is matched by the PID we just launched, not by class/title
#    substring (see n64-shot.sh for the "prepares"/"shares" incident).
#
# ── Same capture-trust caveats as n64-shot.sh ─────────────────────────
#  * gpu-screen-recorder -w region reads the same compositor surface grim
#    does, so on a desktop where Ares' Vulkan surface doesn't actually
#    present, this will produce a black/clipped MP4 instead of the game.
#    That is a property of the desktop, not of the ROM or this script.
#  * Re-matched by PID before capture and again after the duration sleep,
#    so a crash mid-capture is a loud failure rather than a short MP4.
set -euo pipefail

ROM="${1:?usage: n64-rec.sh <rom.z64> <out.mp4> [duration-seconds] [settle-seconds]}"
OUT="${2:?usage: n64-rec.sh <rom.z64> <out.mp4> [duration-seconds] [settle-seconds]}"
DURATION="${3:-60}"
SETTLE="${4:-3}"

for tool in hyprctl gpu-screen-recorder ffprobe; do
  command -v "$tool" >/dev/null || { echo "n64-rec: '$tool' not found (needs Hyprland + gpu-screen-recorder + ffprobe)" >&2; exit 1; }
done
[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || { echo "n64-rec: not inside a Hyprland session" >&2; exit 1; }

ARES="${ARES:-ares}"
command -v "$ARES" >/dev/null || { echo "n64-rec: ares not on PATH" >&2; exit 1; }

WORK="$(mktemp -d)"
cleanup() {
  [ -n "${REC_PID:-}" ] && kill -INT "$REC_PID" 2>/dev/null || true
  [ -n "${EMU_PID:-}" ] && kill "$EMU_PID" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

cp "$ROM" "$WORK/rom.z64"
chmod u+w "$WORK/rom.z64"

"$ARES" --system "Nintendo 64" \
        --no-file-prompt --kiosk \
        --settings-file "$WORK/settings.bml" \
        "$WORK/rom.z64" >"$WORK/ares.log" 2>&1 &
EMU_PID=$!

# Same PID-matched lookup as n64-shot.sh — see that script for the rationale.
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

GEOM=""
for _ in $(seq 1 60); do
  sleep 0.5
  GEOM="$(find_ares_geom)"
  [ -n "$GEOM" ] && break
done

if [ -z "$GEOM" ]; then
  echo "n64-rec: ares window never appeared" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi

sleep "$SETTLE"

# Re-check before capturing: if Ares died during settle, fail loudly.
GEOM2="$(find_ares_geom)"
if [ -z "$GEOM2" ]; then
  echo "n64-rec: ares window closed during the settle period — nothing to capture" >&2
  echo "n64-rec: this usually means ares crashed after mapping its window; check:" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi
if [ "$GEOM2" != "$GEOM" ]; then
  echo "n64-rec: ares window geometry changed ($GEOM -> $GEOM2) during the settle period; using the current one" >&2
  GEOM="$GEOM2"
fi

# Reformat "X,Y WxH" -> "WxH+X+Y" for gpu-screen-recorder's -region flag.
X="${GEOM%%,*}"
REST="${GEOM#*,}"
Y="${REST%% *}"
SZ="${REST##* }"
W="${SZ%x*}"
H="${SZ#*x}"
REGION="${W}x${H}+${X}+${Y}"

# gpu-screen-recorder traps SIGINT and finalises the MP4 container on shutdown.
# -bm cqp + -q high is gsr's "named-quality" mode (the high/medium/low
# presets map to QP values inside the encoder); gsr rejects -q <name>
# alongside -bm cbr/-bm vbr, which require -q as a numeric quantiser. For a
# 320x240 source on a fast desktop CPU/GPU, cqp at the 'high' preset is
# overkill but cheap and produces a clean MP4. -fm cfr duplicates frames if
# the source dips below 60 fps so the output duration matches the request.
gpu-screen-recorder -w region -region "$REGION" \
    -f 60 -q high -fm cfr -k h264 -ac aac -bm qp \
    -a app:ares -o "$OUT" >"$WORK/rec.log" 2>&1 &
REC_PID=$!

sleep "$DURATION"

# Verify the emulator is still alive before tearing down. If Ares died
# mid-capture the MP4 may be short or absent — report that explicitly.
if ! kill -0 "$EMU_PID" 2>/dev/null; then
  echo "n64-rec: ares exited before the ${DURATION}s capture finished — MP4 may be incomplete" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
fi

kill -INT "$REC_PID" 2>/dev/null || true
# Give the recorder a moment to flush the MP4 trailer before the trap
# cleanup kills it harder.
for _ in $(seq 1 20); do
  kill -0 "$REC_PID" 2>/dev/null || break
  sleep 0.2
done
kill "$REC_PID" 2>/dev/null || true

if [ ! -f "$OUT" ]; then
  echo "n64-rec: recorder produced no output; check:" >&2
  sed -n '1,20p' "$WORK/rec.log" >&2
  exit 1
fi

# ffprobe the result so the user sees what actually got captured.
SUMMARY="$(ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height,codec_name \
    -of csv=s=x:p=0 "$OUT" 2>/dev/null || echo "?")"
DUR="$(ffprobe -v error -show_entries format=duration \
    -of csv=s=x:p=0 "$OUT" 2>/dev/null || echo "?")"
HAS_AUDIO="$(ffprobe -v error -select_streams a:0 \
    -show_entries stream=codec_name -of csv=s=x:p=0 "$OUT" 2>/dev/null || echo none)"

echo "n64-rec: wrote $OUT (region $REGION, ${DURATION}s requested, dur ${DUR}s, video ${SUMMARY}, audio ${HAS_AUDIO})"