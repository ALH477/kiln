# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-skel.nix — a rigged .t3dm poses its bind skeleton on the host.
#
# plat/host used to abort on every t3d_skeleton_* call, and its header argued
# for why an abort beat a silent no-op:
#
#   "CLAUDE.md records that a skinned model drawn at the wrong origin cost this
#   project a full pass of camera retuning, because 'cameras aimed at him
#   photographing empty room' reads as a framing problem."
#
# The aborts are gone because the skeleton is real. That turns the argument
# into a claim — the host's bind pose IS the console's bind pose — and this
# check is what makes the claim falsifiable. It is Tiny3D's own arithmetic,
# copied rather than approximated, over a chunk parsed out of the same bytes a
# ROM links, so a disagreement is a bug rather than a difference.
#
# ── The fixture, and why this one ──────────────────────────────────────
# assets/skel_test.gltf (tools/gen_skel_gltf.py): two bones, 'hip' at the
# origin and 'arm' one Blender unit above it, two clips. It is the smallest rig
# that can tell a composed pose from an uncomposed one — on a single-bone rig,
# or on any rig whose bones all sit at the origin, forgetting to multiply by
# the parent is invisible. At --base-scale=32 the child lands at y = 32, which
# is a number the check states rather than a shape it eyeballs.
#
# The same file is already the RIG jump ROM's model (flake.nix's skelModel), so
# it stays exercised on hardware as well as here.
#
# ── Not committed, converted ───────────────────────────────────────────
# Same discipline as kiln-model.nix: the .t3dm is produced by the SAME
# gltf_to_t3d the ROM build uses, so the bytes under test are the bytes a ROM
# would link, and the file cannot go stale silently when the converter moves.
#
# ── What this does NOT check ───────────────────────────────────────────
# Animation. The keyframes live in a .sdata sidecar the host does not stream,
# so t3d_anim_update keeps the console's clock exactly and leaves the bones
# alone. The check asserts BOTH halves of that on purpose: the clock, because
# fig_skel's overlay fade and fig_skel_done read it, and the stillness, so
# that the day host animation lands this check fails loudly rather than
# passing while claiming something it no longer tests.
{ pkgs, target, n64Inst, skelGltf }:

target.mkCheck {
  pname = "skelcheck";
  sources = [ ./kiln-skel-check.c ];
  args = "rig.t3dm out.png";
  meta.description = "a rigged .t3dm poses its bind skeleton on the host";
  # --base-scale=32 matches flake.nix's skelModel, so the 32.0 the check
  # asserts is the same number the RIG jump ROM loads. Changing it here without
  # changing it there would make this check pass about a model nothing boots.
  preRun = ''
    ${n64Inst}/bin/gltf_to_t3d --base-scale=32 --ignore-materials \
      ${skelGltf} rig.t3dm
    echo "converted $(stat -c%s rig.t3dm) bytes of rigged .t3dm"
  '';
  script = ''
    if ! cmp -s out.png ${./refs/kiln-skel.png}; then
      echo ""
      echo "FAILED: the rendered bind pose changed."
      echo "  reference $(stat -c%s ${./refs/kiln-skel.png}) bytes, rendered $(stat -c%s out.png) bytes"
      echo "Every structural and arithmetic assertion above passed, so the"
      echo "skeleton parsed and composed the same way — this is shading,"
      echo "projection, or the per-part matrix push landing in a different"
      echo "order. Magnify before accepting a new reference."
      exit 1
    fi
    echo "the bind pose matches its reference capture (${target.description})"
  '';
}
