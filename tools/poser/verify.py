#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""verify.py — prove the poser's Euler convention against Blender's own bake.

    python3 tools/poser/verify.py dank

── What is being proven ───────────────────────────────────────────────────
tools/poser/src/pose.js claims that a Blender pose-bone XYZ Euler maps to a
glTF node-local quaternion as

    node.quaternion = restQuat * quatFromEulerXYZ(pose)      (RIGHT multiply)

Everything the editor displays rests on that. If it were a LEFT multiply, or
if the Euler order were wrong, the viewport would still show a plausible
character doing plausible things — just not the one the ROM will contain.
That is the exact failure mode this tool was built to end, so it does not get
to assume its own premise.

The proof is a differential test with no second implementation to drift:

  * The SOURCE keyframes come from tools/poser/data/*.json.
  * The bake is goblin.py's own `_sample` and `LAG`, imported, not copied.
  * The expected answer is the curve Blender exported into the .gltf from
    those same keyframes.

── Two different numbers, and only one of them is a bug ───────────────────
The first version of this compared the ideal eased curve against the .gltf at
arbitrary times and reported errors up to 25 degrees. That was the test being
wrong, not the pipeline: `_bake` samples the eased curve every `step` frames
and Blender interpolates LINEARLY between those samples, so mid-step the two
are supposed to differ. So the check splits in two:

  CONVENTION  compared only at frames `_bake` actually keyed (multiples of
              `step`). There the exported value IS our sampled value, so any
              disagreement is a real convention or bake bug. This is what
              passes or fails the run.

  CURVE LOSS  the worst mid-step gap between the ideal eased curve and the
              piecewise-linear one that ships. Reported, never failed — it is
              the cost of `step`, and the honest way to choose it. goblin.py
              picked step=4 by measuring FILE SIZE; this measures what that
              costs in fidelity, which is the other half of the trade.

Run with no argument to check every character.

Exit codes: 0 clean, 1 a convention mismatch, 2 bad usage.
"""

import json
import math
import struct
import sys
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BLENDER = REPO / "tools" / "blender"
sys.path.insert(0, str(BLENDER))
sys.modules.setdefault("bpy", types.ModuleType("bpy"))

TOLERANCE_DEG = 1.5   # exporter quantisation + anim_io's 0.01-degree rounding
BAKE_STEP = 4         # goblin.py `_bake`'s default; keys land on multiples
FPS = 24              # Blender's default scene rate, asserted below

COMPONENT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


# ── minimal glTF reading ───────────────────────────────────────────────────

def load_gltf(path):
    doc = json.loads(Path(path).read_text())
    binpath = Path(path).parent / doc["buffers"][0]["uri"]
    blob = binpath.read_bytes()

    def accessor(i):
        a = doc["accessors"][i]
        view = doc["bufferViews"][a["bufferView"]]
        fmt = COMPONENT[a["componentType"]]
        n = NCOMP[a["type"]]
        off = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        count = a["count"] * n
        return struct.unpack_from(f"<{count}{fmt}", blob, off)

    return doc, accessor


# ── quaternion helpers ─────────────────────────────────────────────────────

def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def quat_from_euler_xyz(deg):
    """Blender's rotation_euler with mode 'XYZ'.

    The order string names the order the rotations are APPLIED, so 'XYZ' is
    X first — which as matrices acting on column vectors composes RIGHT to
    left: R = Rz @ Ry @ Rx.

    That is NOT what three.js's Euler(x, y, z, 'XYZ') builds; three.js's
    'XYZ' is Rx @ Ry @ Rz, and the equivalent of Blender's is three.js's
    'ZYX'. The two agree exactly whenever only one axis is non-zero, which
    is why every single-axis action in this repo verified clean while the
    riding poses — the only ones that rotate a bone about two axes at once —
    were out by up to 26 degrees. Measured, not reasoned:

        three.js 'XYZ' (Rx@Ry@Rz)   worst 25.95 deg
        Blender  'XYZ' (Rz@Ry@Rx)   worst  0.03 deg
    """
    hx, hy, hz = (math.radians(d) * 0.5 for d in deg)
    cx, sx = math.cos(hx), math.sin(hx)
    cy, sy = math.cos(hy), math.sin(hy)
    cz, sz = math.cos(hz), math.sin(hz)
    # Rz * Ry * Rx as a quaternion product.
    return (sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz)


def quat_slerp(a, b, t):
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b = tuple(-x for x in b)
        d = -d
    if d > 0.9995:
        r = tuple(x + (y - x) * t for x, y in zip(a, b))
        n = math.sqrt(sum(c * c for c in r)) or 1.0
        return tuple(c / n for c in r)
    th = math.acos(max(-1.0, min(1.0, d)))
    s = math.sin(th)
    w0, w1 = math.sin((1 - t) * th) / s, math.sin(t * th) / s
    return tuple(x * w0 + y * w1 for x, y in zip(a, b))


def quat_angle_deg(a, b):
    d = abs(sum(x * y for x, y in zip(a, b)))
    return math.degrees(2.0 * math.acos(max(-1.0, min(1.0, d))))


def sample_track(times, values, t):
    n = len(times)
    if n == 0:
        return (0.0, 0.0, 0.0, 1.0)
    if t <= times[0]:
        return tuple(values[0:4])
    if t >= times[-1]:
        return tuple(values[(n - 1) * 4:(n - 1) * 4 + 4])
    i = 0
    while i < n - 1 and times[i + 1] < t:
        i += 1
    u = (t - times[i]) / (times[i + 1] - times[i])
    return quat_slerp(tuple(values[i * 4:i * 4 + 4]),
                      tuple(values[(i + 1) * 4:(i + 1) * 4 + 4]), u)


# ── the check ──────────────────────────────────────────────────────────────

def check_model(model, data_dir, verbose=False):
    gltf_path = data_dir / f"{model}.gltf"
    index_path = data_dir / f"{model}.index.json"
    if not gltf_path.exists():
        print(f"  {model}: no {gltf_path.name} staged — run `./dev poser` "
              f"or nix run .#poser to stage it")
        return None
    if not index_path.exists():
        print(f"  {model}: no {index_path.name} — run "
              f"`python3 tools/blender/anim_io.py dump goblin {model} "
              f"{data_dir}`")
        return None

    import kilnlib          # noqa: F401  (ease lives here, used via goblin)
    import importlib.util
    spec = importlib.util.spec_from_file_location("gob", BLENDER / "goblin.py")
    gob = importlib.util.module_from_spec(spec)
    m = sys.modules["kilnlib"]
    m.make_armature = lambda *a, **k: None
    m.make_skinned_mesh = lambda *a, **k: None
    m.make_mesh = lambda *a, **k: None
    m.make_action = lambda *a, **k: None
    m.reset_scene = lambda: None
    m.report = lambda max_tris=None: None
    m.export_gltf = lambda *a, **k: None
    sys.argv = ["x", "--", "--model", model, "--out", "/dev/null"]
    spec.loader.exec_module(gob)

    doc, accessor = load_gltf(gltf_path)
    nodes = doc["nodes"]
    joints = doc["skins"][0]["joints"]
    node_of = {nodes[j].get("name", f"node{j}"): j for j in joints}
    rest_of = {nodes[j].get("name", f"node{j}"):
               tuple(nodes[j].get("rotation", [0, 0, 0, 1])) for j in joints}

    baked = {}
    for anim in doc.get("animations", []):
        tracks = {}
        for ch in anim["channels"]:
            if ch["target"]["path"] != "rotation":
                continue
            smp = anim["samplers"][ch["sampler"]]
            tracks[ch["target"]["node"]] = (accessor(smp["input"]),
                                            accessor(smp["output"]))
        dur = max((t[0][-1] for t in tracks.values() if t[0]), default=0.0)
        baked[anim["name"]] = (tracks, dur)

    index = json.loads(index_path.read_text())
    worst_all, worst_where = 0.0, ""
    loss_all = [0.0, ""]
    for entry in index["actions"]:
        action = json.loads((data_dir / entry["file"]).read_text())
        name = action["name"]
        if name not in baked:
            print(f"  {model}.{name}: not in the .gltf, skipped")
            continue
        tracks, dur = baked[name]
        keys = [(k["frame"], {b: tuple(v) for b, v in k["bones"].items()},
                 k.get("ease", "inout")) for k in action["keys"]]
        rest = keys[0][1] if keys else {}
        length = action["length"] or 1
        loop = action.get("loop", True)

        # The exporter writes seconds; the source is in frames. These must
        # agree exactly or every comparison below is off by a scale factor.
        if dur > 0 and abs(dur * FPS - length) > 0.51:
            print(f"  {model}.{name}: duration {dur:.4f}s * {FPS} != "
                  f"{length} frames — frame rate assumption is wrong")
            worst_all = 999.0
            continue

        def ours_at(bone, frame):
            lag = gob.LAG.get(bone, 0)
            src = frame - lag
            src = src % length if loop else max(0.0, min(float(length), src))
            pose = gob._sample(keys, src, rest)
            deg = pose.get(bone, rest.get(bone, (0.0, 0.0, 0.0)))
            return quat_mul(rest_of[bone], quat_from_euler_xyz(deg))

        # ── convention: only where _bake actually placed a key ──────────
        worst, wbone, wframe = 0.0, "", 0
        frames = list(range(0, length + 1, BAKE_STEP))
        if frames[-1] != length:
            frames.append(length)
        for frame in frames:
            for bone, node in node_of.items():
                if node not in tracks:
                    continue
                d = quat_angle_deg(ours_at(bone, frame),
                                   sample_track(*tracks[node], frame / FPS))
                if d > worst:
                    worst, wbone, wframe = d, bone, frame

        # ── curve loss: the mid-step cost of `step`, informational ──────
        loss, lbone, lframe = 0.0, "", 0
        for i in range(length * 2 + 1):
            frame = i * 0.5
            for bone, node in node_of.items():
                if node not in tracks:
                    continue
                d = quat_angle_deg(ours_at(bone, frame),
                                   sample_track(*tracks[node], frame / FPS))
                if d > loss:
                    loss, lbone, lframe = d, bone, frame

        flag = "ok " if worst < TOLERANCE_DEG else "FAIL"
        if verbose or worst >= TOLERANCE_DEG:
            print(f"  {flag} {model}.{name:<10} conv {worst:6.3f}deg "
                  f"({wbone} @ f{wframe})   curve-loss {loss:5.2f}deg "
                  f"({lbone} @ f{lframe:.1f})")
        if worst > worst_all:
            worst_all, worst_where = worst, f"{model}.{name} {wbone}"
        if loss > loss_all[0]:
            loss_all[0], loss_all[1] = loss, f"{model}.{name} {lbone}"
    return worst_all, worst_where


def main(argv):
    data_dir = HERE / "data"
    models = argv or ["dank", "sparky", "moss", "glimmer", "goblin"]
    print(f"── poser convention check (tolerance {TOLERANCE_DEG}deg) ──")
    fail = 0
    checked = 0
    for model in models:
        r = check_model(model, data_dir, verbose=len(models) == 1)
        if r is None:
            continue
        checked += 1
        worst, where = r
        status = "ok" if worst < TOLERANCE_DEG else "FAILED"
        print(f"  {model:<9} convention {worst:6.3f}deg  {where:<26} {status}")
        if worst >= TOLERANCE_DEG:
            fail += 1
    if checked == 0:
        print("  nothing staged to check")
        return 2
    print("verify: convention holds" if not fail
          else "verify: FAILED — the editor would misrepresent the rig")
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
