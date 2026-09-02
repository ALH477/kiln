# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-shell.nix — the playable build actually plays.
#
# Every other host gate renders one frame from a harness that calls the engine
# directly. This one runs the REAL thing: examples/engine/main.c, unedited,
# its own blocking for(;;) loop, driven by plat/shell — window creation, event
# pump, frame pacing, audio device negotiation and all — for ninety frames.
#
# ── Why ninety frames and not one ──────────────────────────────────────
# One frame proves the linker found everything. Ninety proves the loop can
# come back round, which is the property that was actually broken: until this
# work `audio_can_write()` returned 1 forever, so kiln_audio_update's drain
# loop never terminated and any host build of a real game hung on frame one.
# Nothing caught it because no gate had ever run a game loop. This is the gate
# that would have.
#
# ── Why statistics and not a reference image ───────────────────────────
# engine-demo's HUD prints a smoothed frame rate read off a real monotonic
# clock, so two runs cannot produce the same PNG and a golden image here would
# be a gate that fails at random. CLAUDE.md records the other half of that
# lesson — anything a capture is diffed against must suppress what cannot be
# the same twice — and the honest answer for a launcher, whose whole job is to
# be subject to real time, is to check the picture statistically instead. A
# frame that is 100% black is the failure this catches, and it is the failure
# that actually happens: a present hook that never fires, a window that never
# gets a surface, a game that stops drawing after the first frame.
#
# SDL's dummy video and audio drivers, because a Nix sandbox has no display —
# every line of the launcher runs except the final handoff to a compositor.
{ pkgs, game }:

pkgs.runCommand "check-kiln-shell"
{
  nativeBuildInputs = [ pkgs.python3 ];
  meta.description = "the real game loop runs under the launcher for 90 frames";
}
  ''
    set -euo pipefail

    export SDL_VIDEODRIVER=dummy
    export SDL_AUDIODRIVER=dummy
    export HOME=$TMPDIR

    ${game}/bin/kiln-engine-demo --frames 90 --shot frame.png --stats | tee stats.txt

    test -s frame.png || { echo "FAILED: no frame was written"; exit 1; }

    python3 - <<'PYEOF'
import re, sys
txt = open("stats.txt").read()

def need(pat, what):
    m = re.search(pat, txt)
    if not m:
        sys.exit("FAILED: the launcher printed no " + what + ":\n" + txt)
    return float(m.group(1))

presented = need(r"shell: presented (\d+) of \d+ frames", "present count")
nonblack = need(r"non-black\s+\d+/\d+\s+\(([0-9.]+)%\)", "non-black percentage")
colours  = need(r"colours\s+(\d+)",        "colour count")
written  = need(r"pixels-written\s+(\d+)", "pixels-written counter")
print("presented %d frames; non-black %.1f%%  colours %d  pixels-written %d"
      % (presented, nonblack, colours, written))

# A Gouraud-shaded cube over a cleared field produces thousands of distinct
# colours; a frame that never got drawn produces about three. That is the
# discriminating number here, not the non-black percentage — engine-demo
# clears to #0a0a18, so "non-black" is ~98% even when nothing is rendered.
# Two independent numbers. The pixel figures come from the backend and say
# the frame was DRAWN; this one comes from the launcher and says it was
# PRESENTED. A dead present hook leaves a perfect framebuffer, so without
# this the gate could only report it by hanging — which it used to do.
fail = []
if presented < 90:  fail.append("only %d of 90 frames reached the present hook" % presented)
if nonblack < 50:  fail.append("only %.1f%% non-black: the clear never happened" % nonblack)
if colours < 500:  fail.append("only %d distinct colours: nothing was shaded" % colours)
if written < 5000: fail.append("only %d pixels written in the last frame" % written)
if fail:
    sys.exit("FAILED: " + "; ".join(fail))
PYEOF

    echo "the launcher ran the real game loop for 90 frames and drew a frame"
    mkdir -p $out && cp frame.png stats.txt $out/
  ''
