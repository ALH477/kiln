# SPDX-License-Identifier: MIT
#
# nix/checks/blender-tests.nix — run the bpy-free builder tests.
#
# tools/blender/test_*.py is the fastest feedback loop in the whole geometry
# pipeline: every builder returns plain (verts, faces, colors) and imports no
# bpy, so a winding mistake, a colour of the wrong arity, a face indexing past
# its vertex array or a model built at ten times its real size is caught in
# milliseconds instead of after a headless Blender launch, a gltf_to_t3d run, a
# ROM build and an emulator capture.
#
# It was also, until this check existed, ungated. `nix flake check` never ran
# any of it. The cost of that was immediate and concrete: `test_rider.py` had
# been failing with `ModuleNotFoundError: No module named 'bpy'` under a bare
# python3 — it imports bpy at module scope where its five siblings stub it —
# and nothing noticed, because nothing ran it. That test does legitimately need
# a live Blender (it evaluates a posed armature), so it is now named
# `blender_test_rider.py`; see the naming contract below. (It has since moved
# to Ganja Goblin's own repo along with rider.py and vehicles.py — the rider
# station it tests is that game's content, not the engine's.)
#
# ── Two globs, deliberately ────────────────────────────────────────────
#   test_*.py           bpy-free. Runs under a bare python3. GATED HERE, and
#                       every one of them must pass.
#   blender_test_*.py   needs a live Blender. NOT run here — Blender is not a
#                       build input for a check (nix/blender.nix's own comment
#                       explains why Blender stays out of the hermetic path
#                       where it can), and a check that silently skipped a
#                       test whose name did not say so is what got us here.
#
# The prefix is what makes the exclusion deliberate rather than accidental.
# To run the Blender-only ones by hand, inside `nix develop`:
#
#   for t in tools/blender/blender_test_*.py; do
#     blender --background --factory-startup -noaudio --python "$t" || break
#   done
#
# ── Why `ls` is asserted against ───────────────────────────────────────
# A glob that matches nothing makes a `for` loop a no-op and this derivation
# succeed having tested exactly nothing. That is the same silent-pass failure
# mode the drift this check exists to catch had, so the count is asserted.
{ pkgs }:

pkgs.runCommand "check-blender-tests"
{
  nativeBuildInputs = [ pkgs.python3 ];
  blenderDir = ../../tools/blender;
  meta.description = "tools/blender's bpy-free builder tests all pass";
}
  ''
    set -euo pipefail
    mkdir -p "$out"

    # Copied out of the store because the tests write nothing but Python does
    # want to sit somewhere it can create __pycache__ without failing.
    cp -rL "$blenderDir" ./blender
    chmod -R u+w ./blender

    # The naming contract, checked first so the diagnosis arrives before the
    # bare ModuleNotFoundError the run loop would otherwise produce.
    for t in ./blender/test_*.py; do
      if grep -q '^import bpy' "$t" \
         && ! grep -q 'sys\.modules\.setdefault("bpy"' "$t"; then
        echo "FAIL: $(basename "$t") imports bpy without stubbing it." \
             "Either stub bpy the way test_goblins.py does, or rename it" \
             "blender_test_*.py so this check skips it on purpose." >&2
        exit 1
      fi
    done

    n=0
    for t in ./blender/test_*.py; do
      name="$(basename "$t")"
      echo "── $name ──"
      python3 "$t" 2>&1 | tee "$out/$name.log"
      n=$((n + 1))
    done

    if [ "$n" -lt 3 ]; then
      echo "FAIL: only $n bpy-free test(s) ran; tools/blender should have at" \
           "least three (goblins, objkit, prims) now that the PetaByte" \
           "Madness- and Ganja-Goblin-specific ones (env, props, world," \
           "vehicles, rider) have moved to those games' own repos. A glob" \
           "that matches nothing makes the loop above a no-op and this" \
           "derivation succeed having tested nothing — the same silent pass" \
           "this check exists to stop." >&2
      exit 1
    fi

    echo "blender builder tests PASSED ($n suites)"
    touch "$out/ok"
  ''
