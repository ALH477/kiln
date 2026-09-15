# SPDX-License-Identifier: MIT
#
# nix/checks/kiln-logic.nix — host assertions over the engine's pure-logic modules.
#
# Same idea as nix/checks/kiln-asset.nix, generalised: several engine modules are
# plain arithmetic over caller-owned structs, touching nothing of libdragon or
# Tiny3D beyond `fm_vec3_t`, `fm_vec3_sub`, `debugf` and `assertf`. Those are
# stubbed in nix/checks/stub/, so the REAL engine sources compile natively and
# can be asserted against in seconds — no cross toolchain, no ROM, no emulator.
#
# ── Why this was worth building ────────────────────────────────────────
# Before it, one engine module out of 46 had a test. Everything else was
# verified by building a ROM and looking at it, which does not work for this
# kind of code: kiln_clip's failure modes render perfectly and read as level
# design mistakes, and kiln_cache's read as a missing asset.
#
# It earned its keep on the first run, finding two real bugs that both `nix
# build` and `nix flake check` had been passing:
#
#   1. kiln_cache: MAKE_HANDLE(0, 0) == 0 == KILN_CACHE_HANDLE_INVALID, so the
#      FIRST resource acquired into a fresh cache returned a handle every
#      caller is told means failure, and which resolve/release/refcount all
#      refuse. The entire first slot was dead, and it failed in the shape of a
#      missing file.
#   2. kiln_clip: the broadphase grid's placement pass wrote brush indices at a
#      running global cursor instead of at each cell's own `offset + count`,
#      discarding the offsets the compaction pass had just computed. Cells
#      were populated with the wrong brushes, so with broadphase on a player
#      walked through two of a four-walled room's walls.
#
# Both are recorded at their fix sites; see kiln_cache.c's HANDLE_INDEX comment
# and kiln_clip.c's placement-pass comment.
#
# ── Which modules, and why not more ────────────────────────────────────
# Sixteen engine modules that both COMPILE and LINK natively. The tier itself
# is engine/modules.mk's HOST_MODULES and is held exact in both directions by
# nix/checks/kiln-parity.nix; most of what HOST_MODULES has that this does
# not — kiln_dialogue, kiln_prof, kiln_sound, kiln_surface, kiln_widget, and
# now the wider IO/asset/audio tier — compile fine but call into the
# graphics, audio or filesystem tiers, so they get the -Werror second opinion
# there and cannot be run standalone here.
#
# The ones whose defects are worth spelling out, because they all render
# plausibly and none of them looks like a bug in a screenshot:
#   kiln_clip    the module PetaByte Madness' whole first-person section rests
#                on; its failures read as level-design mistakes
#   kiln_dict    the .map-text-to-typed-args mapping every spawn goes through
#   kiln_cache   generation-counted handles, i.e. exactly the kind of bit
#                packing that is either right or catastrophically wrong
#   kiln_lod     threshold selection; must be monotonic in distance
#   kiln_rng     seeded determinism, and kiln_dice sits directly on it
#   kiln_stream  the room/tile streaming pacer's priority ordering, budget
#                admission and pool-full eviction — a wrong priority compare
#                renders as "the wrong tile loaded first", indistinguishable
#                from a content/authoring mistake in any capture
#   kiln_voxel   the block grid's two reductions. A greedy box that overlaps
#                its neighbour is a collision brush the player sticks inside,
#                and a dropped surface quad is a hole you can see through —
#                neither distinguishable from level-design intent in a capture.
#                The Tiny3D-facing half is kiln_voxmesh and is deliberately NOT
#                here, which is why the vertex packing was split out of the
#                mesher rather than the other way round.
#
# Deliberately NOT here: anything that talks to the RDP, the RSP, the mixer,
# DFS or a T3DModel. Stubbing those would mean asserting against the stub.
# kiln_scratch and kiln_board are candidates for a follow-on; kiln_asset
# already has its own check, and kiln_streamio (the console-glue half of the
# streaming pacer, binding kiln_stream to real kiln_asset/kiln_cache calls)
# is a heavier follow-on for the same reason kiln_asset's own runtime check
# is separate — it needs real StreamDB content, not just stubs.
#
# ── -Werror, on purpose ────────────────────────────────────────────────
# The engine's own Makefile sets -Wno-error (a deliberate choice for
# third-party header noise on the cross build). Compiling the same sources
# natively at -Werror is a free second opinion, and it immediately surfaced an
# unused parameter in kiln_cache_acquire that the cross build had been
# reporting to nobody for as long as the module has existed.
{ pkgs, engineSrc, hostMath, platHost }:

pkgs.runCommand "check-kiln-logic"
{
  nativeBuildInputs = [ pkgs.gcc ];
  meta.description = "engine pure-logic modules (clip, dict, cache, lod, rng, voxel) asserted on the host";
}
  ''
    set -euo pipefail

    # The host headers must come FIRST on the include path so <libdragon.h>
    # and <t3d/t3dmath.h> resolve to them; the engine sources are then
    # compiled completely unmodified, which is the property that makes this a
    # test of the engine rather than of a fork of it.
    #
    # Three include roots now, and the middle one is the interesting change:
    #   ./stub          <libdragon.h> — still a stand-in, until P2
    #   platHost        <t3d/t3dmath.h> — typedefs only, defines nothing
    #   hostMath        <fgeom.h>/<fmath.h> — libdragon's REAL fast math,
    #                   compiled natively by nix/host-math.nix
    # fm_vec3_t used to be a hand-copy living in ./stub with a header
    # explaining that a copy was the only honest way to fake it. It is not a
    # copy any more, so the caveat is retired: the type and every fm_* result
    # the assertions below run through are the console's, bit for bit, and
    # nix/checks/kiln-hostmath.nix is what holds that true.
    # ── ASan + UBSan, which is most of the point of a host build ────────
    # Neither exists for a bare-metal MIPS target. Until now the only way to
    # learn that the engine had walked off the end of something was a VR4300
    # halt, diagnosed after the fact with mips64-elf-addr2line — and both of
    # the user's boards (ED64 Plus, ModRetro M64) have no cartridge-side USB,
    # so there is not even a debugf to catch on the way down. The sanitizers
    # turn that class of bug from a post-mortem into a stack trace.
    #
    # detect_leaks is on: kiln_cache and kiln_scratch hand out ownership, and a
    # leak on a 4 MB console is a crash a few minutes later somewhere else.
    SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"

    gcc -O1 -g -std=gnu2x -Wall -Wextra -Werror $SAN \
        -I${./stub} -I${platHost}/include -I${hostMath}/include \
        -I${engineSrc}/src/kiln \
        -o kilnlogic_check \
        ${./kiln-logic-check.c} \
        ${engineSrc}/src/kiln/kiln_cache.c \
        ${engineSrc}/src/kiln/kiln_camlint.c \
        ${engineSrc}/src/kiln/kiln_char.c \
        ${engineSrc}/src/kiln/kiln_clip.c \
        ${engineSrc}/src/kiln/kiln_dice.c \
        ${engineSrc}/src/kiln/kiln_dict.c \
        ${engineSrc}/src/kiln/kiln_inventory.c \
        ${engineSrc}/src/kiln/kiln_lod.c \
        ${engineSrc}/src/kiln/kiln_physics.c \
        ${engineSrc}/src/kiln/kiln_radio.c \
        ${engineSrc}/src/kiln/kiln_rng.c \
        ${engineSrc}/src/kiln/kiln_sierp.c \
        ${engineSrc}/src/kiln/kiln_stream.c \
        ${engineSrc}/src/kiln/kiln_tile.c \
        ${engineSrc}/src/kiln/kiln_turn.c \
        ${engineSrc}/src/kiln/kiln_voxel.c \
        ${engineSrc}/src/kiln/kiln_weapon.c \
        ${engineSrc}/src/kiln/kiln_weapons.c \
        ${hostMath}/lib/libkilnmath.a -lm

    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      ./kilnlogic_check

    echo "kiln engine-logic check PASSED"
    mkdir -p $out && touch $out/ok
  ''
