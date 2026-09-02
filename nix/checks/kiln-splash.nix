# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-splash.nix — the engine's real boot splash renders.
#
# Runs the actual kiln_splash_init/update/apply/draw3d/draw2d sequence — not
# a stand-in for it — against a real .t3dm converted from
# tools/blender/kiln_logo.py's own generated glTF, and holds the settled
# frame to a committed reference.
#
# The .gltf comes from the ALREADY-BUILT kilnLogo package rather than
# re-invoking Blender here, so this check does not pay for a second headless
# Blender run; gltf_to_t3d is then run directly on it (bypassing mkModel's
# `mkasset` compression step) for the same reason nix/checks/kiln-model.nix
# does — the host reader has no decompression stage, and does not need one to
# prove the model parses and draws correctly.
#
# ── What a lookup failure would mean, and why it is asserted first ────
# kiln_splash_draw3d looks up "kiln", "flame" and "plate" by name and gives
# the flame its own transform, independent of the body's settle animation —
# see kiln_splash.h's "The flame moves separately from the body". If a future
# edit to kiln_logo.py renames or drops one of those objects, kiln_splash
# falls back to drawing the model as one rigid piece: correct for an older or
# foreign model, wrong for this one, and a plain pixel diff downstream would
# only be able to say "the frame changed" — not why. The check's structural
# assertions catch the specific failure at the specific place it would occur.
#
# ── One body, every architecture ───────────────────────────────────────
# Built by nix/host.nix's `target`: it supplies the compiler, the flags and
# the three archives, so this file says what to render and what to compare
# and nothing about how to compile it. The same body runs under wasm32
# against the SAME reference files — see nix/checks/kiln-wasm.nix.
{ pkgs, target, n64Inst, kilnLogo }:

target.mkCheck {
  pname = "splashcheck";
  sources = [ ./kiln-splash-check.c ];
  args = "kiln_logo.t3dm out.png";
  meta.description = "the real boot splash renders the kiln, its flame, and the lit publisher line";
  preRun = ''
    ${n64Inst}/bin/gltf_to_t3d --ignore-materials \
      ${kilnLogo}/share/gltf/kiln_logo.gltf kiln_logo.t3dm
    echo "converted $(stat -c%s kiln_logo.t3dm) bytes of .t3dm"
  '';
  script = ''
    if ! cmp -s out.png ${./refs/kiln-splash.png}; then
      echo ""
      echo "FAILED: the settled splash frame changed."
      echo "  reference $(stat -c%s ${./refs/kiln-splash.png}) bytes, rendered $(stat -c%s out.png) bytes"
      echo "The structural assertions above passed, so the model still has"
      echo "its three named objects — this is geometry, shading, the"
      echo "flicker phase at this exact frame, or the text glow. Magnify"
      echo "before accepting a new reference — a gate is perfectly happy to"
      echo "freeze a wrong picture forever."
      exit 1
    fi
    echo "the settled splash frame matches its reference capture (${target.description})"
  '';
}
