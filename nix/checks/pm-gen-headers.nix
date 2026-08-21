# SPDX-License-Identifier: MPL-2.0
#
# nix/checks/pm-gen-headers.nix — the generated dimension headers must be
# current, and the generators must still agree with the committed geometry.
#
# PetaByte Madness' modelling rule is "never type a dimension into C that
# geometry already defines", and the mechanism is a generator that measures
# itself and emits a header (tools/blender/pm_world.py -> src/pm_world_gen.h,
# PetaByte-Madness/tools/dank_lab_gen.py -> src/pm_lab_gen.h). A generated file
# checked into the tree is only as good as its regeneration discipline, so this
# check enforces two separate things:
#
# ── 1. The committed header is what the generator emits today ───────────
# Re-run each emitter into a temp file and diff. A generator edited without
# regenerating leaves C deriving from stale numbers, which is the same defect as
# hand-typing them — just harder to notice, because the header LOOKS generated.
#
# ── 2. The generator still agrees with the geometry that actually ships ──
# dank_lab_gen.py is not on the build path: it emitted
# assets/obj/dank_lab.obj once, and that OBJ is what pmProp converts and what
# the ROM contains. So the header could be perfectly current and still describe
# a room nobody draws — which is exactly the failure pm_lab.h's own comment
# records from the other direction ("every consumer of the lab's extents except
# pm_lab.c itself was keyed to a room shape nothing drew").
#
# The bbox is what the runtime keys off — pm_lab.h's PM_LAB_REAL_*, the
# collision brushes, and every camera derived from them — so the bbox is what is
# asserted, to a tenth of a world unit.
#
# Triangle counts are REPORTED, not failed on. They already differ today (the
# generator emits 1622 tris, the committed OBJ has 1538) because the generator
# has been edited since the OBJ was exported. That is worth knowing and is not
# worth blocking a build over: the geometry it changed is set dressing, the room
# is the same size, and nothing in C depends on the triangle count. A check that
# failed here would be a check people learned to regenerate past.
{ pkgs }:

pkgs.runCommand "check-pm-gen-headers"
{
  nativeBuildInputs = [ pkgs.python3 ];
  pmTools = ../../PetaByte-Madness/tools;
  pmSrc = ../../PetaByte-Madness/src;
  pmObj = ../../PetaByte-Madness/assets/obj/dank_lab.obj;
  blenderDir = ../../tools/blender;
  meta.description = "generated dimension headers are current and match the shipped geometry";
}
  ''
    set -euo pipefail
    mkdir -p "$out" work

    echo "── pm_lab_gen.h is current ──"
    python3 "$pmTools/dank_lab_gen.py" --emit-header work/pm_lab_gen.h > work/emit.log
    cat work/emit.log
    if ! diff -u "$pmSrc/pm_lab_gen.h" work/pm_lab_gen.h > work/lab.diff; then
      echo "FAIL: PetaByte-Madness/src/pm_lab_gen.h is out of date." >&2
      echo "  Regenerate it:" >&2
      echo "    python3 PetaByte-Madness/tools/dank_lab_gen.py \\" >&2
      echo "        --emit-header PetaByte-Madness/src/pm_lab_gen.h" >&2
      echo "" >&2
      head -60 work/lab.diff >&2
      exit 1
    fi
    echo "  pm_lab_gen.h matches the generator ✓"

    echo "── the generator and the shipped dank_lab.obj describe one room ──"
    # objkit is the reader pm_props.py itself uses, so this measures the OBJ the
    # same way the build does rather than with a second parser.
    PYTHONPATH="$blenderDir" python3 - "$pmTools/dank_lab_gen.py" "$pmObj" <<'PY'
    import sys, importlib.util
    import objkit

    gen_path, obj_path = sys.argv[1], sys.argv[2]

    spec = importlib.util.spec_from_file_location("dank_lab_gen", gen_path)
    G = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(G)
    G.build()
    m = G.measure()

    obj = objkit.load_obj(obj_path)
    lo, hi = objkit.bbox(obj["verts"])

    # The generator authors Z-up-ish in its own centimetre space and writes the
    # OBJ in the same space, so the axes correspond directly — no conversion.
    pairs = [
        ("X min", m["x0"], lo[0]), ("X max", m["x1"], hi[0]),
        ("Y min", m["y0"], lo[1]), ("Y max", m["y1"], hi[1]),
        ("Z min", m["z0"], lo[2]), ("Z max", m["z1"], hi[2]),
    ]
    # 0.1 world units = 0.16 cm. Tighter than any real edit and looser than
    # float formatting noise.
    TOL_CM = 0.16
    bad = []
    for name, g, o in pairs:
        mark = "ok" if abs(g - o) <= TOL_CM else "DRIFT"
        if mark == "DRIFT":
            bad.append((name, g, o))
        print("  %-6s generator %9.2f   obj %9.2f   %s" % (name, g, o, mark))

    print("  tris    generator %9d   obj %9d   (reported, not gated)"
          % (len(G.TRIS), len(obj["faces"])))

    if bad:
        print("", file=sys.stderr)
        print("FAIL: dank_lab_gen.py and assets/obj/dank_lab.obj disagree about "
              "the room's SIZE:", file=sys.stderr)
        for name, g, o in bad:
            print("  %s: generator %.2f cm vs obj %.2f cm" % (name, g, o),
                  file=sys.stderr)
        print("", file=sys.stderr)
        print("pm_lab.h's PM_LAB_REAL_* (and therefore the collision brushes "
              "and every camera derived from them) come from the GENERATOR, "
              "while the ROM ships the OBJ. A size disagreement means the "
              "runtime is keyed to a room nobody draws. Re-export the OBJ:",
              file=sys.stderr)
        print("  python3 PetaByte-Madness/tools/dank_lab_gen.py "
              "--outdir PetaByte-Madness/assets/obj", file=sys.stderr)
        sys.exit(1)
    PY

    echo "generated-header check PASSED"
    touch "$out/ok"
  ''
