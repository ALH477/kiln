# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-web.nix — the browser launcher runs, and draws.
#
# nix/checks/kiln-gui-wasm32.nix and friends prove the RENDERER is
# architecture-independent: same reference PNG from wasm32 as from x86_64.
# They say nothing about plat/shell/shell_web.c, which is the file that gets
# those pixels onto a canvas, reads the keyboard off the window, and — the
# interesting part — makes the ROM's own blocking `for (;;)` legal in a
# browser by yielding through ASYNCIFY inside display_get.
#
# None of that can be checked by a build. It also cannot be checked by a
# browser here: the Nix sandbox has no display, and even outside it a real
# browser is not something a pre-push gate can depend on. So the launcher is
# linked for node (nix/host.nix's wasm32-node target: same C, same EM_JS, same
# ASYNCIFY, .js instead of .html) and run against nix/checks/kiln-web-dom.js —
# a canvas that records what it was given.
#
# What this proves: the wasm loads, main() runs, ASYNCIFY suspends and resumes
# the game loop repeatedly, the present hook reaches putImageData with a
# correctly sized ImageData, the pixels in it have content, and the launcher
# survives a machine with no AudioContext — which is also every browser tab
# before the user's first gesture. And the Kiln Studio bridge: the frame count
# the launcher publishes matches the canvas's, a console command queued before
# start is run by the real kiln_console (its log comes back holding `help`'s
# output), and queued pad input reaches the pad the launcher pushes into the
# engine. A second run with nothing queued must fail those assertions, so they
# cannot pass on a launcher that never drains the queues.
#
# What it does not prove: compositing, scaling, vsync, or that a gamepad maps
# the way a player expects. Those need a real browser and a person. The
# rendering itself is already gated four architectures deep, so what is left
# unverified here is a page and a scale factor, not a frame.
{ pkgs, target, domStub }:

pkgs.runCommand "check-kiln-web"
{
  nativeBuildInputs = [ pkgs.nodejs pkgs.python3 ];
  meta.description = "the browser launcher loads, loops and draws to a canvas";
}
  ''
    set -euo pipefail
    mkdir -p work && cd work

    node --require ${domStub} \
         ${target}/bin/kiln-engine-demo.js \
         --frames 12 --shot frame.png --stats 2>&1 | tee run.txt

    test -s frame.png || { echo "FAILED: no frame was written"; exit 1; }

    KILN_WEB_DOM_NO_QUEUE=1 node --require ${domStub} \
         ${target}/bin/kiln-engine-demo.js --frames 12 --stats > unqueued.txt 2>&1

    python3 - <<'PYEOF'
import json, re, sys
txt = open("run.txt").read()

def bridge_problems(txt, frames):
    b = re.search(r"dom: bridge frame (-?\d+), cmdq (\d+), padq (\d+), pad buttons (\d+), "
                  r"stick (-?\d+),(-?\d+), console (.*)$", txt, re.M)
    if not b:
        return ["the DOM stub printed no bridge line"]
    frame, cmdq, padq, buttons, sx, sy = (int(g) for g in b.groups()[:6])
    lines = json.loads(b.group(7))
    out = []
    if frame != frames:
        out.append("Module.kiln.frame.n is %d, the canvas got %d frames" % (frame, frames))
    if cmdq:
        out.append("console: %d queued command(s) never taken" % cmdq)
    if not any(l.endswith("> help") for l in lines) or not any("commands:" in l for l in lines):
        out.append("console: `help` never ran (log: %r)" % lines[:4])
    if padq:
        out.append("pad: %d queued input(s) never taken" % padq)
    if not (buttons & 1) or (sx, sy) != (60, -30):
        out.append("pad: the queued A + stick 60,-30 never reached the launcher's pad (buttons %d, stick %d,%d)"
                   % (buttons, sx, sy))
    return out

m = re.search(r"dom: putImageData (\d+) calls, (\d+)x(\d+), "
              r"last frame non-black (\d+), colours (\d+)", txt)
if not m:
    sys.exit("FAILED: the DOM stub reported nothing — the present hook never "
             "reached the canvas:\n" + txt)

calls, w, h, nonblack, colours = (int(g) for g in m.groups())

# The launcher's own count, cross-checked against the DOM's. They are
# measured on opposite sides of the EM_JS boundary, so a disagreement means
# the blit is being dropped between C and the canvas.
p = re.search(r"shell: presented (\d+) of \d+ frames", txt)
presented = int(p.group(1)) if p else -1

print("canvas: %d frames, %dx%d, non-black %d, colours %d"
      % (calls, w, h, nonblack, colours))

fail = []
# 12 frames asked for, 12 frames presented. Fewer means ASYNCIFY did not
# resume the loop, which is the one thing about this backend that has no
# analogue anywhere else in the project.
if calls < 12:              fail.append("only %d of 12 frames reached the canvas" % calls)
if presented != calls:      fail.append("launcher presented %d, canvas got %d" % (presented, calls))
if (w, h) != (320, 240):    fail.append("canvas is %dx%d, not 320x240" % (w, h))
if nonblack < 1000:         fail.append("only %d non-black pixels: nothing was drawn" % nonblack)
if colours < 500:           fail.append("only %d distinct colours: nothing was shaded" % colours)
fail += bridge_problems(txt, calls)
if fail:
    sys.exit("FAILED: " + "; ".join(fail))
print("bridge: frame count, a queued console command and queued pad input all reached the engine")

unq = open("unqueued.txt").read()
u = re.search(r"dom: putImageData (\d+) calls", unq)
missed = bridge_problems(unq, int(u.group(1)) if u else -1)
if not (any(p.startswith("console:") for p in missed) and any(p.startswith("pad:") for p in missed)):
    sys.exit("FAILED: with nothing queued the bridge assertions still passed — they are not checking the drain: %r"
             % missed)
print("bridge: with nothing queued, the same assertions fail (%s)" % "; ".join(missed))
PYEOF

    echo "the browser launcher looped 12 times and drew to a canvas"
    mkdir -p $out && cp frame.png run.txt unqueued.txt $out/
  ''
