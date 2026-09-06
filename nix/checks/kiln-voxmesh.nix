# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-voxmesh.nix — which combiner the voxel atlas survives.
#
# This check was written to PROVE that Forge's atlas never reached the screen.
# It did its job: Forge now sets RDPQ_COMBINER_TEX_SHADE in begin_voxel_state
# (Forge/src/forge_geo.c), so the fix below has LANDED and the second reference
# is what the editor draws. The first is kept as a regression guard — it now
# pins "this is what it looks like when the combiner is wrong" rather than
# "this is what Forge does".
#
# The check renders a real voxel mesh — kiln_voxel's greedy surface extraction
# through kiln_voxmesh's packer through the host 3D pass — TWICE, with two
# colour combiners, and diffs both captures. The pair is the point:
#
#   refs/kiln-voxmesh-shade.png     the WRONG combiner: a featureless white
#                                   slab. Five different block types,
#                                   indistinguishable, and you cannot even see
#                                   where one block ends and the next begins.
#   refs/kiln-voxmesh-texshade.png  the same geometry with the atlas sampled.
#                                   This is Forge today.
#
# NOTE this check sets both combiners ITSELF and never compiles or reads
# Forge, so it cannot detect a regression in Forge's own choice. It pins what
# the two combiners DO, not what Forge picks. If someone drops the
# rdpq_mode_combiner call from begin_voxel_state, this check stays green and
# the editor silently goes back to grey.
#
# ── The chain, all of it verifiable from the sources ──────────────────
#   kiln_voxmesh.c:103-106   block type N -> atlas tile N-1, via the UVs. The
#                            type goes NOWHERE else.
#   kiln_voxmesh.c:108       vertex colour is DIR_SHADE[dir] — greyscale
#                            per-face brightness, carrying no type at all.
#   forge_geo.c              sets T3D_FLAG_TEXTURED, so the RSP emits texture
#                            coordinates — and now also sets the combiner.
#   kiln_engine.c:126        kiln_scene_begin sets RDPQ_COMBINER_SHADE, every
#                            frame. Correct as the engine's untextured default;
#                            Forge overrides it for its own mesh.
#   tiny3d t3d.c:300         t3d_state_set_drawflags only encodes the RSP
#                            triangle command. It does not touch the combiner.
#
# Under the wrong combiner every one of Forge's fifteen block types drew the
# same grey, PAINT mode's CI4 atlas never reached the framebuffer, and `Z`'s
# veiled-palette preview could not change the geometry it was previewing. The
# counter kiln_host_t3d_counters()->texels_discarded measures it: ~83,000
# texels sampled and thrown away in one frame, which is what the -shade
# reference still records.
#
# ── What the fix was ──────────────────────────────────────────────────
# The fix is one rdpq_mode_combiner call and it belongs in Forge, not the
# engine: kiln_voxmesh_draw's own comment says "Sets NO render state: the
# caller has already chosen the combiner", so the engine behaves as designed
# and the caller is the one omitting it. Which combiner, and whether the
# authored palettes still read once it lands, is a judgement about a CRT that
# no host capture settles — it is exactly what Forge's PAINT mode exists to
# answer, on hardware — and it is STILL unanswered: the combiner landed, but
# nothing has been booted, so whether the authored ramp separates once the veil
# discards hue remains a judgement no host capture settles.
#
# ── One body, every architecture ───────────────────────────────────────
# Built by nix/host.nix's `target`: it supplies the compiler, the flags and
# the three archives, so this file says what to render and what to compare
# and nothing about how to compile it. The same body runs under wasm32
# against the SAME reference files — flake.nix declares that variant as
# `<name>-wasm32`; `./dev arch <target>` runs it on the others.
{ pkgs, target, forgeSrc }:

target.mkCheck {
  pname = "voxcheck";
  sources = [ ./kiln-voxmesh-check.c ];
  args = "shade.png texshade.png";
  meta.description = "which combiner the voxel atlas survives";
  script = ''
    fail=0
    for pair in "shade.png ${./refs/kiln-voxmesh-shade.png}" \
                "texshade.png ${./refs/kiln-voxmesh-texshade.png}"; do
      set -- $pair
      if ! cmp -s "$1" "$2"; then
        echo "  FAILED: $1 differs from its reference"
        echo "    reference $(stat -c%s "$2") bytes, rendered $(stat -c%s "$1") bytes"
        fail=1
      fi
    done
    if [ $fail -ne 0 ]; then
      echo ""
      echo "If Forge now sets a TEX combiner, the -shade reference is obsolete"
      echo "and that is the good news — regenerate both and look at them."
      exit 1
    fi
    echo "both voxel renders match their references (${target.description})"

    # The half this check could not do until now. It sets both combiners
    # ITSELF, so it pins what the two combiners DO and is blind to which one
    # Forge picks -- drop the call from begin_voxel_state and every capture
    # here still matches while the editor silently goes back to grey. That is
    # the same shape as the drift this check was written to expose, one level
    # up, so assert the caller as well as the callee.
    if ! grep -q 'rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE)'          ${forgeSrc}/forge_geo.c; then
      echo ""
      echo "FAILED: Forge/src/forge_geo.c no longer sets a TEX combiner."
      echo "  kiln_voxmesh puts the block type ONLY in the UVs, so without it"
      echo "  kiln_scene_begin's RDPQ_COMBINER_SHADE stands, the texel is"
      echo "  discarded, and all fifteen block types draw the same grey."
      echo "  That is refs/kiln-voxmesh-shade.png, which is still green above"
      echo "  because this check renders both combiners itself."
      exit 1
    fi
    echo "and Forge still chooses the one that samples the atlas"
  '';
}
