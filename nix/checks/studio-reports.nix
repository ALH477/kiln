# SPDX-License-Identifier: MIT
#
# nix/checks/studio-reports.nix — every validator the studio shows speaks
# tools/schema/report.schema.json, on good input and on bad.
#
# The dashboard renders reports generically, so a validator whose --json drifts
# — a lowercase code, `ok` true beside an error, a missing `metrics` — would show
# the wrong colour rather than fail. This runs each one on something it passes
# and something it must reject, holds both outputs to the schema with
# tools/schema/report_check.py (whose own selftest runs first), and asserts the
# verdicts: the good case ok, the bad case not.
#
#   camlint        a clean shot / a shot with keys out of order
#   map-validate   quake_test.map / a three-plane "brush" that closes nothing,
#                  through the studio's adapter from mapfmt's own JSON
#   asset-budget   (bad only) a budget with no scenes — the good case is
#                  checks.asset-budget-demo, which stages real models
#   poser-verify   (bad only) nothing staged in the sandbox
#   gait           the shipped goblin glTF; a measurement, so ok either way
{ pkgs, goblinModel, toolsDir, camlint, quakeMap }:

pkgs.runCommand "check-studio-reports"
{
  nativeBuildInputs = [ pkgs.python3 ];
  meta.description = "every Kiln Studio validator's --json conforms to the report schema, good and bad input";
}
  ''
    set -euo pipefail
    cp -r ${toolsDir} tools
    chmod -R u+w tools
    mkdir -p "$out"
    python3 tools/schema/report_check.py --selftest | tee "$out/selftest.txt"

    python3 - ${camlint}/bin/camlint ${quakeMap} ${goblinModel}/share/gltf/goblin.gltf <<'EOF' | tee "$out/report.txt"
    import json, subprocess, sys, tempfile, os
    sys.path.insert(0, "tools/schema")
    sys.path.insert(0, "tools/studio")
    from report_check import problems
    from studio.validators import adapt_map

    camlint, quake_map, goblin = sys.argv[1:4]
    fails = 0

    def expect(cond, msg):
        global fails
        print(("  ok   " if cond else "  FAIL ") + msg)
        if not cond:
            fails += 1

    def judge(label, report, want_ok):
        found = problems(report)
        expect(not found, f"{label}: conforms" + (f" — {found}" if found else ""))
        if want_ok is not None and isinstance(report, dict):
            expect(report.get("ok") is want_ok, f"{label}: ok is {report.get('ok')}, want {want_ok}")

    def tool_json(argv):
        r = subprocess.run(argv, capture_output=True, text=True)
        try:
            return json.loads(r.stdout)
        except ValueError:
            return {"_unparseable": r.stdout[-300:] + r.stderr[-300:]}

    def tmp(text, suffix):
        fd, path = tempfile.mkstemp(suffix=suffix)
        with os.fdopen(fd, "w") as f:
            f.write(text)
        return path

    print("── camlint ──")
    judge("clean shot", tool_json([camlint, "tools/camlint/fixtures/clean.shot.json", "--json"]), True)
    judge("keys out of order", tool_json([camlint, "tools/camlint/fixtures/broken-time-order.shot.json", "--json"]), False)

    print("── map-validate, through the studio's adapter ──")
    judge("quake_test.map", adapt_map(tool_json([sys.executable, "tools/mapmaker/validate.py", quake_map, "--json"])), True)
    open_brush = tmp('{\n"classname" "worldspawn"\n{\n'
                     '( 0 0 0 ) ( 0 1 0 ) ( 1 0 0 ) FORGE1 0 0 0 1 1\n'
                     '( 0 0 0 ) ( 1 0 0 ) ( 0 0 1 ) FORGE1 0 0 0 1 1\n'
                     '( 0 0 0 ) ( 0 0 1 ) ( 0 1 0 ) FORGE1 0 0 0 1 1\n}\n}\n', ".map")
    judge("a brush that closes nothing", adapt_map(tool_json([sys.executable, "tools/mapmaker/validate.py", open_brush, "--json"])), False)

    print("── asset-budget ──")
    budget = tmp(json.dumps({"platform": {}, "all_assets": [], "scenes": {}}), ".json")
    root = tempfile.mkdtemp()
    judge("a budget with no scenes", tool_json([sys.executable, "tools/asset_budget.py", "check", "--budget", budget,
                                                "--root", root, "--json"]), False)

    print("── poser-verify ──")
    judge("nothing staged", tool_json([sys.executable, "tools/poser/verify.py", "goblin", "--json"]), False)

    print("── gait ──")
    g = tool_json([sys.executable, "tools/blender/gait.py", goblin, "Walk", "Run", "--json"])
    judge("the shipped goblin", g, True)
    expect("Walk" in g.get("metrics", {}) and g["metrics"]["Walk"].get("ground_speed", 0) > 0,
           "gait reports Walk's ground speed")

    print("studio-reports: " + ("FAILED" if fails else "ok"))
    sys.exit(1 if fails else 0)
    EOF
  ''
