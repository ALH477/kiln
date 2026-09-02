# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-gui.nix — the 2D pass renders, and renders the same.
#
# This is the gate CLAUDE.md records as impossible: "screenshot verification is
# not part of nix flake check — it needs a live Wayland session. It is a ./dev
# command, run on a desktop." That was true of an emulator capture. It is not
# true of the host 2D pass, which is a software rasteriser: no session, no
# driver, no compositor, and byte-identical output across runs — so a reference
# image actually means something. See plat/host/src/host_gfx.c for why it is
# software and not OpenGL.
#
# ── What is under test ────────────────────────────────────────────────
# engine/src/kiln/kiln_gui.c itself, UNMODIFIED, linked against the host
# backend. Not a port of it and not a mock of it — the same translation unit
# the ROM links. Every primitive is exercised, because each takes a different
# path through the shim: rect and bar are fill_rectangle under COMBINER_FLAT,
# panel is four one-pixel edges over a body, line is two COMBINER_SHADE
# triangles with the blender on, and text goes through the builtin font that
# nix/checks/kiln-font.nix extracted out of libdragon's own blob.
#
# ── Why two reference files ───────────────────────────────────────────
# See nix/checks/refs/README.md. Short version: the PNG catches geometry, the
# manifest catches text, and a pixel diff is the wrong instrument for text.
#
# ── What this does NOT prove ──────────────────────────────────────────
# That the console draws the same pixels. It cannot: there is no RDP here, the
# 2D pass is being reimplemented rather than emulated, and fill rate — the
# console's actual binding constraint — has no host analogue at all. What it
# proves is that kiln_gui's own arithmetic is stable and that its output is
# what a human looked at once and approved. The console remains the arbiter of
# appearance; ./dev shot is still how you find out what a frame really looks
# like.
#
# ── One body, every architecture ───────────────────────────────────────
# This check is built by nix/host.nix's `target`, which supplies the compiler,
# the flags and the three archives. That is what lets nix/checks/kiln-wasm.nix
# run THIS check, unchanged, against the SAME two reference files under a
# wasm32 build — a second blessed reference per architecture would only prove
# each architecture agrees with itself.
{ pkgs, target }:

target.mkCheck {
  pname = "guicheck";
  sources = [ ./kiln-gui-check.c ];
  args = "out.png out.txt";
  meta.description = "the host 2D pass renders a HUD, byte-identically";
  script = ''
    fail=0
    if ! cmp -s out.txt ${./refs/kiln-gui-hud.txt}; then
      echo ""
      echo "FAILED: the text manifest changed."
      diff -u ${./refs/kiln-gui-hud.txt} out.txt || true
      fail=1
    fi
    if ! cmp -s out.png ${./refs/kiln-gui-hud.png}; then
      echo ""
      echo "FAILED: the rendered framebuffer changed."
      echo "  reference: $(stat -c%s ${./refs/kiln-gui-hud.png}) bytes"
      echo "  rendered : $(stat -c%s out.png) bytes"
      echo "The manifest above says whether text moved; if it did not, the"
      echo "difference is geometry. Magnify both before accepting a new"
      echo "reference — see nix/checks/refs/README.md."
      fail=1
    fi
    [ $fail -eq 0 ] || exit 1

    echo "host 2D pass matches its reference capture and manifest (${target.description})"
  '';
}
