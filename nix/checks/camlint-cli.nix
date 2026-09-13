# SPDX-License-Identifier: MIT
#
# nix/checks/camlint-cli.nix — `camlint` (tools/camlint/camlint.c) reads a shot
# file the way kiln_camlint reads a table.
#
# A clean shot must pass, and one shot broken per hard-failure flag must fail
# with THAT flag's code — so the CLI's JSON reading, its flag-to-code mapping and
# its exit status are each held, in both directions. Malformed input must be a
# usage error (exit 2), never a silent pass. Every --json report is held to
# tools/schema/report.schema.json. The committed fixtures the studio offers are
# run too, so they cannot drift from what the CLI accepts.
{ pkgs, camlint, fixtures, reportCheck }:

pkgs.runCommand "check-camlint-cli"
{
  nativeBuildInputs = [ pkgs.python3 ];
  meta.description = "camlint passes a clean shot, fails one shot per ERR_ flag with its code, and rejects malformed input";
}
  ''
    set -euo pipefail
    mkdir -p "$out"
    cd "$out"
    python3 - ${camlint}/bin/camlint ${fixtures} ${reportCheck} <<'EOF' | tee report.txt
    import json, subprocess, sys, tempfile, os

    camlint, fixtures, report_check = sys.argv[1:4]
    fails = 0

    def expect(cond, msg):
        global fails
        print(("  ok   " if cond else "  FAIL ") + msg)
        if not cond:
            fails += 1

    def run_text(text, *extra):
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as f:
            f.write(text)
        return subprocess.run([camlint, path, *extra], capture_output=True, text=True)

    def schema_ok(stdout):
        r = subprocess.run([sys.executable, report_check, "-"], input=stdout, capture_output=True, text=True)
        return r.returncode == 0, r.stdout.strip()

    base = {"name": "s", "duration": 4.0, "near_z": 4.0, "far_z": 360.0, "loop": False,
            "keys": [{"t": 0.0, "eye": [0, 20, -60], "look": [0, 8, 0]},
                     {"t": 4.0, "eye": [20, 20, -40], "look": [0, 8, 0]}]}

    def with_keys(keys, **kw):
        return {**base, **kw, "keys": keys}

    k = lambda t, eye, look: {"t": t, "eye": eye, "look": look}
    cases = {
        None: json.dumps(base),
        "ERR_NOKEYS": json.dumps(with_keys([])),
        "ERR_ORDER": json.dumps(with_keys([k(0, [0, 20, -60], [0, 8, 0]), k(3, [10, 20, -50], [0, 8, 0]),
                                           k(1, [20, 20, -40], [0, 8, 0])])),
        "ERR_PASTEND": json.dumps(with_keys([k(0, [0, 20, -60], [0, 8, 0]), k(6, [20, 20, -40], [0, 8, 0])])),
        "ERR_DEGEN": json.dumps(with_keys([k(0, [5, 5, 5], [5, 5, 5]), k(4, [6, 5, 5], [0, 5, 5])])),
        "ERR_FRUSTUM": json.dumps({**base, "near_z": 4.0, "far_z": 2.0}),
        "ERR_SUBJCUT": json.dumps({**base, "far_z": 10.0}),
        # JSON has no infinity literal; 1e999 is a valid number that overflows to one.
        "ERR_NAN": json.dumps(with_keys([k(0, [0, 20, -60], [0, 8, 0]), k(4, [12345.5, 20, -40], [0, 8, 0])]))
                       .replace("12345.5", "1e999"),
    }

    print("── one shot per hard failure ──")
    for code, text in cases.items():
        human = run_text(text)
        js = run_text(text, "--json")
        try:
            report = json.loads(js.stdout)
        except ValueError:
            report = {}
        codes = [e.get("code") for e in report.get("errors", [])]
        good, why = schema_ok(js.stdout)
        expect(good, f"{code or 'clean'}: --json conforms to the report schema {why}")
        if code is None:
            expect(human.returncode == 0 and js.returncode == 0 and report.get("ok") is True and not codes,
                   f"a clean shot passes (exit {human.returncode}, codes {codes})")
        else:
            expect(human.returncode == 1 and js.returncode == 1 and report.get("ok") is False and code in codes,
                   f"{code}: exit {js.returncode}, codes {codes}")
            expect("FAIL" in human.stdout, f"{code}: the human report says FAIL")

    print("── malformed input is a usage error, not a pass ──")
    for label, text in (("truncated JSON", "{\"duration\": 4"), ("no duration", json.dumps({**base, "duration": "x"})),
                        ("a key without an eye", json.dumps(with_keys([{"t": 0, "look": [0, 0, 0]}]))),
                        ("trailing garbage", json.dumps(base) + " ]")):
        r = run_text(text)
        expect(r.returncode == 2, f"{label}: exit {r.returncode}")

    print("── the committed fixtures ──")
    clean = subprocess.run([camlint, os.path.join(fixtures, "clean.shot.json"), "--json"], capture_output=True, text=True)
    expect(clean.returncode == 0 and json.loads(clean.stdout)["ok"], "clean.shot.json passes")
    broken = subprocess.run([camlint, os.path.join(fixtures, "broken-time-order.shot.json"), "--json"],
                            capture_output=True, text=True)
    bcodes = [e["code"] for e in json.loads(broken.stdout)["errors"]]
    expect(broken.returncode == 1 and "ERR_ORDER" in bcodes and "ERR_DEGEN" in bcodes,
           f"broken-time-order.shot.json fails with ERR_ORDER and ERR_DEGEN ({bcodes})")

    print("camlint-cli: " + ("FAILED" if fails else "ok"))
    sys.exit(1 if fails else 0)
    EOF
  ''
