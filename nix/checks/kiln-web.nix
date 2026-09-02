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
# before the user's first gesture.
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

    python3 - <<'PYEOF'
import re, sys
txt = open("run.txt").read()

m = re.search(r"dom: putImageData (\d+) calls, (\d+)x(\d+), "
              r"last frame non-black (\d+), colours (\d+)", txt)
if not m:
    sys.exit("FAILED: the DOM stub reported nothing — the present hook never "
             "reached the canvas:\n" + txt)

calls, w, h, nonblack, colours = (int(g) for g in m.groups())
print("canvas: %d frames, %dx%d, non-black %d, colours %d"
      % (calls, w, h, nonblack, colours))

fail = []
# 12 frames asked for, 12 frames presented. Fewer means ASYNCIFY did not
# resume the loop, which is the one thing about this backend that has no
# analogue anywhere else in the project.
if calls < 12:              fail.append("only %d of 12 frames reached the canvas" % calls)
if (w, h) != (320, 240):    fail.append("canvas is %dx%d, not 320x240" % (w, h))
if nonblack < 1000:         fail.append("only %d non-black pixels: nothing was drawn" % nonblack)
if colours < 500:           fail.append("only %d distinct colours: nothing was shaded" % colours)
if fail:
    sys.exit("FAILED: " + "; ".join(fail))
PYEOF

    echo "the browser launcher looped 12 times and drew to a canvas"
    mkdir -p $out && cp frame.png run.txt $out/
  ''
