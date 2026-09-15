# SPDX-License-Identifier: MIT
"""gait.py — measure a rig's locomotion clips the way the floor sees them.

    python3 tools/blender/gait.py <model.gltf> [Clip ...]     # what ships
    python3 tools/blender/gait.py --source goblin [Clip ...]   # goblin.py's keys
    add --json for a tools/schema/report.schema.json report (Kiln Studio reads it)

A walk cycle is judged by its feet, and every defect that matters in one is
invisible in a table of angles:

  * SLIDE. A game moves the character's transform at some speed; the clip
    moves the planted foot backwards at some other speed. The difference is
    the skate. So a clip has a GROUND SPEED — how fast its planted foot
    travels backwards — and the game must play the clip at
    (its own speed / that ground speed). This measures it, in metres per
    second of clip time, from the shipped glTF, at the clip's REAL duration
    (Blender exports at the scene rate, 24 fps, which nothing sets).
  * DRAG. The swing foot must clear the ground while the planted one carries
    the weight. A passing pose that bends the wrong knee plants the swing foot
    and lifts the stance one, and reads as a limp or a shuffle. Measured as the
    swing foot's clearance at mid-swing.
  * FLOAT. With no root translation channel, a knee bend raises the foot
    instead of lowering the hips. Measured as the planted foot's lowest point
    across the cycle — how far the character hovers.

Ground contact is two points per foot, HEEL and TOE, taken at rest from the
rig's own foot bone: the heel straight under the ankle, the toe at the foot
bone's tail. The lower of a foot's two points is its contact height; the foot
with the lower contact is planted.

Two sources, one measurement. --source runs goblin.py's action builders with
the bakers replaced by recorders (the tools/blender/anim_io.py trick) and poses
the same rest skeleton through `rest * euler(XYZ)` — the composition
tools/poser/verify.py proves against Blender's own export. That is what makes
it possible to author a clip and measure it before paying for a Blender run.
"""
import importlib.util
import json
import math
import struct
import sys
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
FPS = 24          # Blender's default scene rate; see the module docstring
RATE = 60         # sample the cycle at the console's frame rate

# ── quaternions (x, y, z, w) ────────────────────────────────────────────────


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qrot(q, v):
    return qmul(qmul(q, (v[0], v[1], v[2], 0.0)), qconj(q))[:3]


def nlerp(a, b, u):
    if sum(x * y for x, y in zip(a, b)) < 0:
        b = tuple(-x for x in b)
    r = tuple(x + (y - x) * u for x, y in zip(a, b))
    n = math.sqrt(sum(c * c for c in r)) or 1.0
    return tuple(c / n for c in r)


def euler_xyz(deg):
    """Blender rotation_euler, mode XYZ: R = Rz @ Ry @ Rx (see verify.py)."""
    hx, hy, hz = (math.radians(d) * 0.5 for d in deg)
    cx, sx = math.cos(hx), math.sin(hx)
    cy, sy = math.cos(hy), math.sin(hy)
    cz, sz = math.cos(hz), math.sin(hz)
    return (sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz)


# ── the rig, as the glTF states it ─────────────────────────────────────────

class Rig:
    def __init__(self, gltf_path):
        p = Path(gltf_path)
        self.doc = json.loads(p.read_text())
        self.blob = (p.parent / self.doc["buffers"][0]["uri"]).read_bytes()
        d = self.doc
        self.nodes = d["nodes"]
        self.joints = d["skins"][0]["joints"]
        self.index = {self.nodes[j]["name"]: j for j in self.joints}
        self.parent = {c: i for i, n in enumerate(self.nodes) for c in n.get("children", [])}
        self.order = []

        def walk(i):
            self.order.append(i)
            for c in self.nodes[i].get("children", []):
                walk(c)
        for r in d["scenes"][0]["nodes"]:
            walk(r)

    def accessor(self, i):
        comp = {5126: "f", 5123: "H", 5125: "I", 5121: "B", 5122: "h", 5120: "b"}
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4}
        a = self.doc["accessors"][i]
        v = self.doc["bufferViews"][a["bufferView"]]
        n = width[a["type"]]
        vals = struct.unpack_from(f"<{a['count'] * n}{comp[a['componentType']]}", self.blob,
                                  v.get("byteOffset", 0) + a.get("byteOffset", 0))
        return vals, n

    def rest(self, i, path):
        n = self.nodes[i]
        return tuple(n.get(path, {"translation": [0, 0, 0], "rotation": [0, 0, 0, 1],
                                  "scale": [1, 1, 1]}[path]))

    def world(self, local):
        """local(node, path) -> value. Returns {node: (t, r, s)} in model space."""
        out = {}
        for i in self.order:
            t, r, s = local(i, "translation"), local(i, "rotation"), local(i, "scale")
            if i in self.parent and self.parent[i] in out:
                pt, pr, ps = out[self.parent[i]]
                t = tuple(a + b for a, b in zip(pt, qrot(pr, tuple(x * k for x, k in zip(t, ps)))))
                r = qmul(pr, r)
                s = tuple(a * b for a, b in zip(ps, s))
            out[i] = (t, r, s)
        return out

    def clips(self):
        return [a["name"] for a in self.doc.get("animations", [])]

    def gltf_clip(self, name):
        anim = next(a for a in self.doc["animations"] if a["name"] == name)
        tracks = {}
        for ch in anim["channels"]:
            smp = anim["samplers"][ch["sampler"]]
            times, _ = self.accessor(smp["input"])
            vals, n = self.accessor(smp["output"])
            tracks[(ch["target"]["node"], ch["target"]["path"])] = (times, vals, n)
        dur = max(t[-1] for t, _, _ in tracks.values())

        def local_at(t):
            def local(i, path):
                if (i, path) not in tracks:
                    return self.rest(i, path)
                ts, v, n = tracks[(i, path)]
                if t <= ts[0]:
                    return tuple(v[:n])
                if t >= ts[-1]:
                    return tuple(v[-n:])
                k = max(j for j in range(len(ts)) if ts[j] <= t)
                u = (t - ts[k]) / (ts[k + 1] - ts[k])
                a, b = tuple(v[k * n:(k + 1) * n]), tuple(v[(k + 1) * n:(k + 2) * n])
                if path == "rotation":
                    return nlerp(a, b, u)
                return tuple(x + (y - x) * u for x, y in zip(a, b))
            return local
        return dur, local_at


# ── goblin.py's source keys ────────────────────────────────────────────────

def source_clips(model="goblin"):
    """{name: (length_frames, loop, pose_at(frame) -> {bone: euler})}."""
    sys.modules.setdefault("bpy", types.ModuleType("bpy"))
    sys.path.insert(0, str(HERE))
    import kilnlib as m
    for fn in ("make_armature", "make_skinned_mesh", "make_mesh", "make_action",
               "reset_scene", "export_gltf"):
        setattr(m, fn, lambda *a, **k: None)
    m.report = lambda max_tris=None: None

    argv = sys.argv
    sys.argv = ["blender", "--", "--model", model, "--out", "/dev/null"]
    spec = importlib.util.spec_from_file_location(f"gait_{model}", HERE / "goblin.py")
    gob = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gob)

    got = {}

    def record(armature, name, keys, length, rest=None, loop=True, step=4):
        rest = rest or {}

        def pose_at(frame):
            out = {}
            for bone in {b for k in keys for b in k[1]}:
                lag = gob.LAG.get(bone, 0)
                src = frame - lag
                src = src % length if loop else max(0.0, min(float(length), src))
                out[bone] = gob._sample(keys, src, rest).get(bone, rest.get(bone, gob.ZERO))
            return out
        got[name] = (length, loop, pose_at)

    sys.argv = argv
    gob._bake = record
    gob.build_all_actions(None, model)
    return got


def source_local(rig, pose):
    def local(i, path):
        if path == "rotation":
            name = rig.nodes[i].get("name")
            deg = pose.get(name)
            if deg is not None:
                return qmul(rig.rest(i, "rotation"), euler_xyz(deg))
        return rig.rest(i, path)
    return local


# ── the measurement ────────────────────────────────────────────────────────

def contact_points(rig, foot):
    """Heel and toe of `foot` in the foot bone's own space, from the rest pose:
    the heel on the ground under the ankle, the toe at the bone's tail."""
    rest = rig.world(lambda i, p: rig.rest(i, p))
    ft, fr, _ = rest[rig.index[foot]]
    inv = qconj(fr)
    heel_w = (ft[0], 0.0, ft[2])
    # The tail is where the foot bone's +Y axis ends; its length is the rest
    # distance to the ground plane along that axis.
    axis = qrot(fr, (0.0, 1.0, 0.0))
    length = ft[1] / -axis[1] if axis[1] < -1e-6 else 0.3
    toe_w = tuple(ft[k] + axis[k] * length for k in range(3))
    heel = qrot(inv, tuple(heel_w[k] - ft[k] for k in range(3)))
    toe = qrot(inv, tuple(toe_w[k] - ft[k] for k in range(3)))
    return heel, toe


def measure(rig, local_at, duration, feet=("foot_l", "foot_r")):
    """Ground speed, stride, drag and float over one cycle.

    Ground speed is taken from the BACKWARD travel of each foot, not from
    "whichever foot is lower": a heel strike dips the heel below the toe of
    the planted foot, so height alone flips between feet at every contact.
    A foot moving backwards relative to the body is on the ground by
    definition of walking forwards; its backward distance over the time it
    spends moving backwards is how fast the ground passes under it.
    """
    pts = {f: contact_points(rig, f) for f in feet}
    n = max(2, int(round(duration * RATE)))
    dt = duration / n
    frames = []
    for k in range(n):
        w = rig.world(local_at(k * dt))
        row = {}
        for f in feet:
            t, r, _ = w[rig.index[f]]
            ps = [tuple(t[j] + v[j] for j in range(3)) for v in (qrot(r, p) for p in pts[f])]
            row[f] = (min(p[1] for p in ps), t[2])
        frames.append(row)

    back_dist = back_time = 0.0
    low, clear = 1e9, 0.0
    for k in range(n):
        a, b = frames[k], frames[(k + 1) % n]
        for f in feet:
            dz = b[f][1] - a[f][1]
            if 0.0 < dz < 0.25:           # backwards, and not a one-shot's wrap
                back_dist += dz
                back_time += dt
        hs = [a[f][0] for f in feet]
        low = min(low, min(hs))
        clear = max(clear, max(hs) - min(hs))
    speed = back_dist / back_time if back_time > 0 else 0.0
    return {
        "duration": duration,
        "ground_speed": speed,                    # m/s the ground passes under a planted foot
        "stride": back_dist / len(feet),          # m one foot pushes back per cycle
        "duty": back_time / (len(feet) * duration),
        "float": low,
        "clearance": clear,
    }


def main(argv):
    as_json = "--json" in argv
    argv = [a for a in argv if a != "--json"]
    results = {}
    emit = (lambda name, r: results.__setitem__(name, r)) if as_json else report
    subject = None
    status = _main(argv, emit)
    if as_json:
        subject = argv[1] if argv and argv[0] == "--source" else (argv[0] if argv else "")
        import json
        # Measurements, not verdicts: goblin-gait.nix owns the thresholds, so
        # this reports what a floor sees and fails nothing.
        print(json.dumps({"tool": "gait", "version": 1, "ok": True, "errors": [], "notes": [],
                          "subject": str(subject),
                          "metrics": {n: {k: round(v, 4) for k, v in r.items()} for n, r in results.items()}},
                         indent=1))
    return status


def _main(argv, report):
    if argv and argv[0] == "--source":
        model = argv[1]
        names = argv[2:]
        gltf = None
        if "--rig" in names:
            k = names.index("--rig")
            gltf = Path(names[k + 1])
            names = names[:k] + names[k + 2:]
        gltf = gltf or next((HERE.parent / "poser" / "data").glob(f"{model}.gltf"), None)
        if gltf is None:
            raise SystemExit("gait: --source needs a rest skeleton: --rig <model.gltf>, or "
                             "tools/poser/data/<model>.gltf from ./dev poser-stage")
        rig = Rig(gltf)
        src = source_clips(model)
        for name in names or sorted(src):
            length, loop, pose_at = src[name]
            dur = length / FPS
            r = measure(rig, lambda t: source_local(rig, pose_at(t * FPS)), dur)
            report(name, r)
        return 0

    rig = Rig(argv[0])
    for name in argv[1:] or rig.clips():
        dur, local_at = rig.gltf_clip(name)
        report(name, measure(rig, local_at, dur))
    return 0


def report(name, r):
    print(f"  {name:<10} {r['duration']:5.3f}s  ground {r['ground_speed']:.3f} m/s  "
          f"stride {r['stride']:.3f} m  duty {r['duty']:.2f}  lowest {r['float']:+.3f} m  "
          f"clearance {r['clearance']:.3f} m")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
