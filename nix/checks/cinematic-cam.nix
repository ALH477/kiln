# SPDX-License-Identifier: MIT
#
# nix/checks/cinematic-cam.nix — examples/cinematic-demo's camera table,
# validated and flown against its own cast on the host.
#
# The demo's shots were once authored for a world sixty times smaller than the
# models drawn into it, so every camera sat inside a model or a wall. The cast's
# positions are pure functions of time (cine_script.h), so the whole loop can
# be sampled here instead of booted. See cinematic-cam-check.c.
#
# The script half diffs the bounds the C header states against each model's
# exported glTF, so a regenerated model that changes size fails here rather than
# walking through a wall on console.
{ pkgs, target, models }:

target.mkCheck {
  pname = "cinematiccamcheck";
  sources = [ ./cinematic-cam-check.c ];
  # engine/src so the ROM's own <kiln/kiln_camkey.h> spelling resolves here too.
  extraCFlags = [ "-I${../../examples/cinematic-demo}" "-I${../../engine/src}" ];
  # tee, not a redirect: under pipefail a failing run still prints its FAILs.
  args = "| tee report.txt";
  extraNativeBuildInputs = [ pkgs.python3 ];
  script = ''
    python3 - report.txt ${models.interceptor} ${models.goblin} ${models.droid} ${models.alien} <<'EOF'
    import json, sys, glob, os
    report, roots = sys.argv[1], sys.argv[2:]
    stated = {}
    for line in open(report):
        if line.startswith("BOUNDS "):
            p = line.split()
            stated[p[1]] = [float(x) for x in p[2:8]]
    bad = 0
    for root in roots:
        for f in glob.glob(os.path.join(root, "share", "gltf", "*.gltf")):
            name = os.path.basename(f)[:-5]
            g = json.load(open(f))
            mn = [1e9] * 3; mx = [-1e9] * 3
            for mesh in g["meshes"]:
                for prim in mesh["primitives"]:
                    a = g["accessors"][prim["attributes"]["POSITION"]]
                    for i in range(3):
                        mn[i] = min(mn[i], a["min"][i]); mx[i] = max(mx[i], a["max"][i])
            got = mn + mx
            want = stated.get(name)
            if want is None:
                print("  FAIL: cine_script.h states no bounds for", name); bad += 1; continue
            worst = max(abs(a - b) for a, b in zip(got, want))
            print("bounds %-12s glTF %s  header %s  worst %.3f m" % (
                name, [round(v, 3) for v in got], want, worst))
            if worst > 0.01:
                print("  FAIL: %s's measured bounds moved; update CINE_B_* in cine_script.h" % name)
                bad += 1
    if len(stated) != 4:
        print("  FAIL: expected 4 BOUNDS lines, got", len(stated)); bad += 1
    sys.exit(1 if bad else 0)
    EOF
  '';
  meta.description = "cinematic-demo: camlint-clean keys, no eye inside the cast, bounds match the models";
}
