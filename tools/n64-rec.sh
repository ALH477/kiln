#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# tools/n64-rec.sh — boot a ROM in Ares on Hyprland and capture a video clip.
#
# Usage: n64-rec.sh <rom.z64> <out.mp4> [duration-seconds] [settle-seconds]
#
# Set NATIVE=1 for a "90s grade" master: the capture is reduced to the N64's
# actual 320x240 output and then point-upscaled back to 4x (1280x960) with
# nearest-neighbour, so every pixel on screen is one console pixel drawn as a
# hard 4x4 block. See the NATIVE stage at the bottom of this file for why
# that is a downscale followed by an upscale rather than just a crop.
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
  echo "n64-rec: ares window never appeared" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi

# ── The workspace trap ────────────────────────────────────────────────
# gpu-screen-recorder's -w region records a SCREEN RECTANGLE off the
# composited output, exactly as grim does — it has no concept of a window
# either. Ares opening on workspace 1 while the monitor shows workspace 3
# therefore yields a full-length, correctly-sized MP4 of workspace 3.
#
# That failure mode has already cost this project real time in the still
# path (see n64-shot.sh's note on two ROMs capturing an identical 64
# non-black pixels), and it is worse here: a 45-second video of the wrong
# thing looks far more convincing than a wrong PNG. So move Ares onto the
# visible workspace and then assert it, rather than trusting the dispatch.
AWS="$(active_ws)"
EWS="$(ares_ws)"
if [ -n "$AWS" ] && [ -n "$EWS" ] && [ "$AWS" != "$EWS" ]; then
  echo "n64-rec: ares is on workspace $EWS, screen is showing $AWS — moving it"
  hyprctl dispatch movetoworkspacesilent "$AWS,pid:$EMU_PID" >/dev/null 2>&1 || true
  hyprctl dispatch focuswindow "pid:$EMU_PID" >/dev/null 2>&1 || true
  sleep 1
  GEOM="$(find_ares_geom)"
fi

sleep "$SETTLE"

AWS="$(active_ws)"
EWS="$(ares_ws)"
if [ -n "$AWS" ] && [ -n "$EWS" ] && [ "$AWS" != "$EWS" ]; then
  echo "n64-rec: ares is on workspace $EWS but the screen shows $AWS." >&2
  echo "n64-rec: the recorder would capture the wrong workspace — refusing." >&2
  exit 1
fi

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

# ── NATIVE: the 90s-grade master ──────────────────────────────────────
# The N64 renders 320x240. Ares presents that scaled up to whatever the
# window is, with its own filtering and (because the window is rarely 4:3)
# its own letterboxing. A capture of that window is neither the console's
# resolution nor its aspect.
#
# So this does three things, in this order, and the order is the point:
#
#   1. CROP to the largest 4:3 region in the middle of the capture. Ares'
#      pillar/letterbox bars are not content and scaling them in would
#      squash the picture.
#   2. SCALE DOWN to 320x240 with `area`. This throws away Ares' upscaling
#      and gets back to one sample per console pixel; `neighbor` here would
#      alias badly because it is a >3x reduction, which is the one place a
#      point filter is the wrong choice.
#   3. SCALE UP 4x with `neighbor`. Now every console pixel is a hard 4x4
#      block — "point-upscaled, so the pixel count you see is the pixel
#      count you get."
#
# ── The encoder settings are not decoration ───────────────────────────
# A point-upscale is only worth doing if the codec does not then smear it.
# Measured on a 1256x776 synthetic capture, counting 4x4 blocks that are
# NOT a single flat value (i.e. pixels the upscale should have made
# identical, and the encoder did not):
#
#   -crf 16 (default psy-rd)              5.30% not flat    651 KB
#   -crf 12 -tune animation               3.24% not flat    790 KB
#   -crf 10 psy-rd=0 deblock=-3,-3        0.04% not flat    750 KB
#   -crf 0  (lossless)                    0.00% not flat   4561 KB
#
# x264's psychovisual optimisation deliberately adds detail near hard
# edges, which is exactly wrong for content whose whole point is hard
# edges — it is the single biggest term here, bigger than the quantiser.
# Turning it off with deblocking down gets within noise of lossless for a
# sixth of the size. The scaler itself is exact: the same chain written
# straight to PNG measures 0.00%.
#
# Audio is copied, not re-encoded: the jingle and the ambience bed are the
# one thing in this capture that is already exactly right.
if [ "${NATIVE:-0}" = "1" ]; then
  NATIVE_OUT="${OUT%.mp4}-320x240.mp4"
  echo "n64-rec: rendering 90s-grade master -> $NATIVE_OUT"
  if ffmpeg -y -v error -i "$OUT" \
      -vf "crop='min(iw,ih*4/3)':'min(ih,iw*3/4)',scale=320:240:flags=area,scale=1280:960:flags=neighbor,setsar=1" \
      -c:v libx264 -preset slow -crf 10 \
      -x264-params psy-rd=0:deblock=-3,-3 -pix_fmt yuv420p \
      -c:a copy "$NATIVE_OUT" 2>"$WORK/native.log"; then
    NSUM="$(ffprobe -v error -select_streams v:0 \
        -show_entries stream=width,height -of csv=s=x:p=0 "$NATIVE_OUT" 2>/dev/null || echo "?")"
    echo "n64-rec: wrote $NATIVE_OUT (${NSUM}, 320x240 native point-upscaled 4x)"
  else
    echo "n64-rec: native pass failed; the raw capture at $OUT is still good" >&2
    sed -n '1,10p' "$WORK/native.log" >&2
  fi
fi