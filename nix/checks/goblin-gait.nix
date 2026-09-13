# SPDX-License-Identifier: MIT
#
# nix/checks/goblin-gait.nix — the goblin's clips, measured the way the floor
# sees them, and the C that plays them held to the measurement.
#
# Every defect this guards against builds cleanly and renders plausibly:
#   * a clip played at the wrong rate skates — the planted foot slides — and
#     the rate a game needs is (its travel speed / the clip's ground speed),
#     a number nobody can read off a table of angles;
#   * clip durations are REAL seconds at Blender's 24 fps scene rate, which
#     nothing sets, so a "40-frame" walk is 1.667 s and not 0.667 s — the
#     cinematic once stepped 2.5x faster than its feet for exactly this reason;
#   * a passing pose that bends the wrong knee drags the swing foot along the
#     ground (clearance near zero);
#   * an attack keyed on a leg is silently thrown away by the overlay mask.
#
# tools/blender/gait.py does the forward kinematics over the SHIPPED glTF
# (the one gltf_to_t3d converts), and its --source mode over goblin.py's own
# keys; the two must agree, which is what proves the FK is the rig's.
{ pkgs, goblinModel, toolsDir, examplesDir }:

pkgs.runCommand "check-goblin-gait"
{
  nativeBuildInputs = [ pkgs.python3 ];
  meta.description = "goblin clips: ground speed, clearance, durations, and the demos' published gait constants";
}
  ''
    set -euo pipefail
    cp -rL ${toolsDir} ./tools
    chmod -R u+w ./tools

    python3 - ${goblinModel}/share/gltf/goblin.gltf ${examplesDir} <<'EOF'
    import re, sys
    from pathlib import Path
    sys.path.insert(0, "tools/blender")
    import gait

    gltf, examples = sys.argv[1], Path(sys.argv[2])
    rig = gait.Rig(gltf)
    fail = 0

    def check(cond, msg):
        global fail
        print(("  ok   " if cond else "  FAIL ") + msg)
        if not cond:
            fail += 1

    # ── what ships ────────────────────────────────────────────────────────
    want = {"Idle": 60, "Walk": 40, "Run": 16, "Jump": 12, "Fall": 24, "Land": 14,
            "Attack": 12, "Roll": 14, "Wave": 50, "Taunt": 60}
    have = set(rig.clips())
    measured = {}
    for name, frames in want.items():
        if name not in have:
            check(False, f"{name}: not in the glTF")
            continue
        dur, local_at = rig.gltf_clip(name)
        check(abs(dur * gait.FPS - frames) < 0.51,
              f"{name}: {dur:.3f} s is {frames} frames at {gait.FPS} fps")
        measured[name] = gait.measure(rig, local_at, dur)

    walk, run = measured.get("Walk"), measured.get("Run")
    if walk and run:
        for n, r in (("Walk", walk), ("Run", run)):
            gait.report(n, r)
        check(0.3 < walk["ground_speed"] < 1.2, f"Walk ground speed {walk['ground_speed']:.3f} m/s is a walk")
        check(run["ground_speed"] > 2.0 * walk["ground_speed"],
              f"Run ground speed {run['ground_speed']:.3f} m/s is at least twice Walk's")
        # The drag test: the swing foot must clear the planted one. Zero means
        # the passing pose bent the wrong knee and both feet pass at one height.
        check(walk["clearance"] > 0.08, f"Walk swing clearance {walk['clearance']:.3f} m")
        check(run["clearance"] > 0.08, f"Run swing clearance {run['clearance']:.3f} m")

    # ── the source agrees with the export: the FK is the rig's ─────────────
    src = gait.source_clips("goblin")
    for name in ("Walk", "Run"):
        length, loop, pose_at = src[name]
        r = gait.measure(rig, lambda t: gait.source_local(rig, pose_at(t * gait.FPS)), length / gait.FPS)
        g = measured[name]["ground_speed"]
        check(abs(r["ground_speed"] - g) < 0.05 * g,
              f"{name}: source {r['ground_speed']:.3f} m/s vs glTF {g:.3f} m/s")

    # ── the attack is upper body only ─────────────────────────────────────
    sys.argv = ["x", "--", "--model", "goblin", "--out", "/dev/null"]
    import importlib.util
    spec = importlib.util.spec_from_file_location("gob", "tools/blender/goblin.py")
    gob = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gob)
    parent = {b: p for b, p, _, _ in gob.BONES}
    def under_torso(b):
        while b is not None:
            if b == "torso":
                return True
            b = parent[b]
        return False
    stray = [b for b in gob.ATTACK_BONES if not under_torso(b)]
    check(not stray, f"Attack keys only bones under the torso mask (stray: {stray})")

    # ── the C that plays them ─────────────────────────────────────────────
    # A demo publishes the ground speed it scales playback by. Holding it to
    # the measurement is what stops a clip edit from quietly reintroducing the
    # skate everywhere it is played.
    pat = re.compile(r"#define\s+GOBLIN_(WALK|RUN)_MPS\s+([0-9.]+)f?")
    seen = 0
    for c in sorted(examples.glob("*/*.[ch]")):
        for m in pat.finditer(c.read_text(errors="replace")):
            seen += 1
            clip = m.group(1).title()
            v, g = float(m.group(2)), measured[clip]["ground_speed"]
            check(abs(v - g) <= 0.03 * g,
                  f"{c.parent.name}/{c.name}: GOBLIN_{m.group(1)}_MPS {v} vs measured {g:.3f}")
    print(f"  {seen} published gait constant(s) checked")

    # The pivot and hip heights the C rotates and places him by.
    pat = re.compile(r"#define\s+GOBLIN_(HIP|ROLL_PIVOT)_M\s+([0-9.]+)f?")
    for c in sorted(examples.glob("*/*.[ch]")):
        for m in pat.finditer(c.read_text(errors="replace")):
            v = float(m.group(2))
            ref = gob.HIP_M if m.group(1) == "HIP" else gob.ROLL_PIVOT_M
            check(abs(v - ref) < 1e-3, f"{c.parent.name}/{c.name}: GOBLIN_{m.group(1)}_M {v} vs goblin.py {ref}")

    sys.exit(1 if fail else 0)
    EOF
    mkdir -p $out && touch $out/ok
  ''
