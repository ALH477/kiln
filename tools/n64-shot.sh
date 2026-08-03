#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# tools/n64-shot.sh — boot a ROM in Ares on Hyprland and capture its window.
#
# Usage: n64-shot.sh <rom.z64> <out.png> [settle-seconds]
#
# ── Why a real display and not a headless one ───────────────────────────
# Ares needs a GPU context; under SDL's dummy video driver the Vulkan-backed
# N64 renderers cannot create a surface at all (gopher64 fails outright with
# "Vulkan support is either not configured in SDL or not available in current
# video driver (dummy)"). Rather than fight that, this drives the compositor
# that is already running and screenshots the window — which is also closer to
# what you actually want to look at.
#
# ── Things learned the hard way ─────────────────────────────────────────
#  * The ROM must be copied somewhere WRITABLE first. Handed a path in the Nix
#    store, Ares pops a modal ("The current save path is read-only") and sits
#    there, so the capture is of a dialog rather than the game.
#  * --settings-file points Ares at a throwaway config. Without it a screenshot
#    run would mutate the user's real Ares settings.
#  * --kiosk removes the menu bar so the capture is just the emulated screen.
set -euo pipefail

ROM="${1:?usage: n64-shot.sh <rom.z64> <out.png> [settle-seconds]}"
OUT="${2:?usage: n64-shot.sh <rom.z64> <out.png> [settle-seconds]}"
SETTLE="${3:-8}"

for tool in hyprctl grim; do
  command -v "$tool" >/dev/null || { echo "n64-shot: '$tool' not found (needs Hyprland + grim)" >&2; exit 1; }
done
[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || { echo "n64-shot: not inside a Hyprland session" >&2; exit 1; }

ARES="${ARES:-ares}"
command -v "$ARES" >/dev/null || { echo "n64-shot: ares not on PATH" >&2; exit 1; }

WORK="$(mktemp -d)"
cleanup() { [ -n "${EMU_PID:-}" ] && kill "$EMU_PID" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

cp "$ROM" "$WORK/rom.z64"
chmod u+w "$WORK/rom.z64"

"$ARES" --system "Nintendo 64" \
        --no-file-prompt --kiosk \
        --settings-file "$WORK/settings.bml" \
        "$WORK/rom.z64" >"$WORK/ares.log" 2>&1 &
EMU_PID=$!

# Find the Ares client's geometry, or print nothing.
#
# Matched by PID, not by class/title substring. An earlier version matched
# any window whose class+title contained "ares" — which also matches, say, a
# browser tab titled "...prepares..." or "...shares...". That silently
# grabbed an unrelated window (once, an open YouTube video) instead of Ares
# and nobody noticed until the "capture" was screenshotted and read. PID is
# unambiguous: exactly one client can belong to the process this script just
# launched. Falls back to an exact (not substring) class match only if no
# window ever reports that PID, e.g. if $ARES resolves to a launcher that
# execs a different final process. `mapped` + `not hidden` is required, not
# just presence in the list: Hyprland can carry a stale entry for a client
# that already died.
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
  echo "n64-shot: ares window never appeared" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi

sleep "$SETTLE"

# Re-check right before capturing, not just at match time. Ares can map a
# window and then die seconds later (a GPU/driver failure after startup is
# the case that was actually observed: Vulkan device open failed, the window
# closed during the settle sleep, and Hyprland's tiling layout reflowed some
# OTHER window into the now-vacant screen region — grim has no concept of
# "the window I meant", only screen coordinates, so it happily screenshotted
# whatever was there. Re-matching here turns that into a loud failure instead
# of a silently wrong PNG.
GEOM2="$(find_ares_geom)"
if [ -z "$GEOM2" ]; then
  echo "n64-shot: ares window closed during the settle period — nothing to capture" >&2
  echo "n64-shot: this usually means ares crashed after mapping its window; check:" >&2
  sed -n '1,20p' "$WORK/ares.log" >&2
  exit 1
fi
if [ "$GEOM2" != "$GEOM" ]; then
  echo "n64-shot: ares window geometry changed ($GEOM -> $GEOM2) during the settle period; using the current one" >&2
  GEOM="$GEOM2"
fi

grim -g "$GEOM" "$OUT"
echo "n64-shot: wrote $OUT (window $GEOM, settled ${SETTLE}s)"
