# SPDX-License-Identifier: MPL-2.0
#
# nix/checks/pm-rigs.nix — the rig JSONs are generated, so regenerate and diff.
#
# PetaByte Madness' two animated characters are described by
# PetaByte-Madness/assets/rig/{horner,machine_centaur}.json, and both files are
# OUTPUT: ph_rig_export.py and mc_rig_export.py build them from ph_rig.py +
# ph_anim_clips.py and machine_centaur_gen.py respectively. They are checked
# into the tree because nix/blender.nix consumes them as a source, which is
# fine — and is exactly the arrangement pm-gen-headers.nix already exists to
# police for the generated C headers, under the rule that "a generated header
# checked into the tree is only as good as its regeneration discipline".
#
# The rigs had no such check. Editing a clip's choreography and forgetting to
# re-run the exporter produced a tree that built, converted with zero dropped
# channels, passed every gate, and shipped the OLD animation — with the new
# source sitting right there in the diff looking applied. There is no symptom
# to notice: the character animates, just not the way the source says.
#
# ── Three things, in the order they can fail ──────────────────────────────
# 1. The CONVENTION proof. Each exporter's --verify poses every bone at every
#    keyframe under both the generator's own maths and the Blender convention
#    the JSON declares, and asserts they agree. The n64-animation skill calls
#    this the first of three layers, and it also ran nowhere in `nix flake
#    check` until now — the same "verified in principle, gated nowhere" state
#    blender-tests.nix was written to fix, and which had let a broken test sit
#    unnoticed. Note --verify runs on every invocation of these scripts, so
#    step 2 re-proves it; it is called out separately here so a convention
#    failure reports as one instead of as a confusing diff.
#
# 2. The REGENERATION diff. Rebuild both JSONs into a scratch directory and
#    compare against the committed ones.
#
# 3. The INVARIANTS the C depends on. These are the ones a diff cannot catch,
#    because a consistently-regenerated file can still be wrong for the game:
#
#      * sit_down and stand_up are 45 frames. pm_intake.c's T_SIT_DOWN and
#        T_STAND_UP are 0.75f "to match", those two feed INTRO_T, INTRO_T sets
#        the intake camera's key times, and those keys are what pm_cine_lint
#        validates. Re-timing a clip silently walks the whole cutscene's
#        camera — and nothing downstream would say so.
#      * climb_in ends on its final pose and is the pose pm_intake.c's
#        MOTOR/IN beats expect to find him already in; a one-shot freezes on
#        its last frame, so a clip that ends somewhere else hands off with a
#        pop.
#      * A looping clip opens and closes on the same pose, or the loop has a
#        seam every cycle.
#
# Cheap: no Blender, no cross compiler, a bare python3 and under a second.
{ pkgs }:

pkgs.runCommand "check-pm-rigs"
{
  # numpy, because ph_rig.py builds its mesh with it. Still no Blender: that
  # is the whole reason these exporters were written to run under a bare
  # python3, and it is what keeps this check under a second.
  nativeBuildInputs = [ (pkgs.python3.withPackages (ps: [ ps.numpy ])) ];
  pmTools = ../../PetaByte-Madness/tools;
  pmRigs = ../../PetaByte-Madness/assets/rig;
  pmLabH = ../../PetaByte-Madness/src/pm_lab.h;
  meta.description = "PetaByte Madness' rig JSONs regenerate identically";
}
  ''
    set -euo pipefail
    mkdir -p "$out"

    cp -rL "$pmTools" ./tools
    chmod -R u+w ./tools

    # ── 1. the convention proofs ──────────────────────────────────────────
    echo "── convention ──"
    python3 ./tools/ph_rig_export.py --verify | tee "$out/horner-verify.log"
    python3 ./tools/mc_rig_export.py --verify | tee "$out/centaur-verify.log"

    # ── 2. regenerate and diff ────────────────────────────────────────────
    echo "── regeneration ──"
    mkdir -p ./gen
    python3 ./tools/ph_rig_export.py --out ./gen/horner.json >/dev/null
    python3 ./tools/mc_rig_export.py --out ./gen/machine_centaur.json >/dev/null

    for r in horner machine_centaur; do
      if ! diff -u "$pmRigs/$r.json" "./gen/$r.json" > "$out/$r.diff"; then
        echo "FAIL: assets/rig/$r.json is STALE." >&2
        echo "  Its generator produces different output than the file in the" >&2
        echo "  tree, so the ROM ships animation that does not match the" >&2
        echo "  source. Regenerate it:" >&2
        echo "" >&2
        echo "    cd PetaByte-Madness" >&2
        echo "    python3 tools/''${r%%_*}*_rig_export.py --out assets/rig/$r.json" >&2
        echo "" >&2
        head -60 "$out/$r.diff" >&2
        exit 1
      fi
      echo "  $r.json is current"
    done

    # ── 3. the invariants pm_intake.c depends on ──────────────────────────
    echo "── invariants ──"
    python3 - "$pmRigs/horner.json" "$pmLabH" <<'PY' | tee "$out/invariants.log"
import json, re, sys

rig = json.load(open(sys.argv[1]))

# Where the model origin sits above the soles. The exporter rebases the mesh
# onto the root bone so the game can place him by the hip (see
# ph_rig_export.py), and pm_lab.h has to state the same number in world units
# to stand him on a floor. Drawn against the wrong one he floats or sinks by
# very nearly a metre, and the only symptom is cutscene cameras aimed at him
# photographing an empty room — which is exactly how this was found.
WORLD_PER_M = 64.0                     # gltf metres -> world (blender.nix baseScale)
src = open(sys.argv[2]).read()
m = re.search(r'#define\s+PM_HORNER_HIP_Y\s+\(([-0-9.]+)f\s*\*\s*([-0-9.]+)f\)', src)
if not m:
    raise SystemExit("FAIL: pm_lab.h no longer defines PM_HORNER_HIP_Y as "
                     "(<cm> * <scale>) — this check parses that form.")
c_world = float(m.group(1)) * float(m.group(2))
rig_world = rig["origin_height"] * WORLD_PER_M
if abs(c_world - rig_world) > 1e-3:
    raise SystemExit(
        "FAIL: pm_lab.h's PM_HORNER_HIP_Y is %.4f world units but the rig's "
        "origin sits %.4f above the soles (%.4f m x %g). Drawing him at the "
        "wrong one floats or sinks him by that difference."
        % (c_world, rig_world, rig["origin_height"], WORLD_PER_M))
print("  %-16s origin %.2f world units above the soles, matches pm_lab.h"
      % ("horner", rig_world))
anims = {a["name"]: a for a in rig["anims"]}
fail = []

def pose_at(anim, frame):
    """Every bone's rotation at `frame`, as the comparable tuple. A bone with
    no key exactly at `frame` is not posed there by this clip."""
    out = {}
    for bone, keys in anim["tracks"].items():
        for k in keys:
            if k["frame"] == frame:
                out[bone] = tuple(round(v, 6) for v in k["rot"])
    return out

# Lengths pm_intake.c restates as seconds. See this file's header for the
# chain that ends at the intake camera's keyframes.
for name, want in (("sit_down", 45), ("stand_up", 45)):
    got = anims[name]["length"]
    if got != want:
        fail.append(
            "%s is %d frames, but pm_intake.c's T_%s is %.2ff = %d frames. "
            "Change both or neither — that constant feeds INTRO_T, which sets "
            "the intake camera's key times."
            % (name, got, name.upper(), want / 60.0, want))
    else:
        print("  %-16s %d frames, matches pm_intake.c" % (name, got))

# A one-shot freezes on its last frame, and pm_intake.c's MOTOR/IN beats
# expect climb_in to have left him in its final pose.
ci = anims["climb_in"]
last = max(k["frame"] for keys in ci["tracks"].values() for k in keys)
if last != ci["length"]:
    fail.append("climb_in's last key is at %d but its length is %d"
                % (last, ci["length"]))
else:
    print("  %-16s ends on its last frame (%d)" % ("climb_in", last))

# A loop that does not close pops once per cycle.
for name, a in anims.items():
    if not a.get("loop"):
        continue
    first, final = pose_at(a, 0), pose_at(a, a["length"])
    if first != final:
        diff = sorted(set(first) ^ set(final)) or sorted(
            b for b in first if first[b] != final.get(b))
        fail.append("%s loops but frame 0 != frame %d (bones: %s)"
                    % (name, a["length"], ", ".join(diff[:6])))
    else:
        print("  %-16s loop closes (%d bones)" % (name, len(first)))

if fail:
    print("")
    for f in fail:
        print("FAIL: " + f, file=sys.stderr)
    raise SystemExit(1)
print("rig invariants OK")
PY

    echo "pm rigs PASSED"
    touch "$out/ok"
  ''
