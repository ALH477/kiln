# SPDX-License-Identifier: MIT
"""anim_io.py — the interchange format between Blender and tools/poser.

    python3 tools/blender/anim_io.py dump goblin dank tools/poser/data
    python3 tools/blender/anim_io.py list goblin dank

── The format, and why it is the SPARSE one ───────────────────────────────
goblin.py's actions are written as animator keyframes — a handful of poses
with an easing mode on each — and `_bake` expands them into the dense linear
keys the glTF exporter wants. There are therefore two candidate interchange
formats, and only one of them is worth editing:

  * the BAKED curves (every 4 frames, every bone, easing already resolved).
    This is what the .gltf contains. It is the right thing to *play* and the
    wrong thing to *edit* — moving one pose means touching sixteen keys.
  * the SOURCE keyframes. Four or five poses per action, each with a name for
    its easing. This is what an animator actually manipulates.

So the JSON is the source keyframes, byte-for-byte what `_bake` takes:

    {
      "name":   "Pose",
      "length": 72,
      "loop":   false,
      "keys": [
        {"frame": 0,  "ease": "inout", "bones": {"arm_r": [0, 0, 74], ...}},
        {"frame": 26, "ease": "out",   "bones": {...}}
      ]
    }

Rotations are XYZ Euler in DEGREES, in Blender pose-bone space, which is the
same space goblin.py's tables are written in — so a value here and a value
there mean the same thing, and a round trip is the identity.

── How the actions get out of Python ──────────────────────────────────────
They are not data; they are code. `anim_ride_hit` calls `_ride(...)` which
merges deltas over a base pose, `anim_walk` calls `_step_pose` twice with
mirrored arguments. Parsing that out of the source would be guesswork.

Instead this RUNS the builders with `_bake` and `make_action` replaced by
recorders. Whatever the functions actually compute is what gets written,
including every helper, every merge and every delta — there is no second
implementation to drift.

That is also why this file imports the model script rather than the other way
round: the model script stays the source of truth and does not learn about
JSON at all unless someone passes --anim-dir.
"""

import json
import sys
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The model scripts import bpy at module scope but only touch it inside
# functions — the same property tools/blender/test_goblins.py relies on.
sys.modules.setdefault("bpy", types.ModuleType("bpy"))


def _round(v):
    """Keep the JSON readable. A tenth of a degree is far below what the
    importer's 60 Hz resample can express, let alone what anyone can see."""
    return [round(float(c), 2) for c in v]


def capture_rig(rig_path):
    """Read source keyframes straight out of a rig JSON.

    A downstream game's own characters did not hold their clips in code the
    way goblin.py does — the clips arrived in assets/rig/<name>.json, produced
    by that game's own exporter tools, and the Blender script's job was only
    to build an armature and replay them. So for a character like that the
    JSON IS the source-keyframe form this module exists to expose, and
    running the builder to recover it would be recovering it from itself.

    It also cannot be run: their build_armature() does real bpy work on the
    object kilnlib.make_armature returns, and the stub returns None. Stubbing far
    enough to fake a posable armature would mean reimplementing Blender.

    ── The layout differs, so it is transposed here ─────────────────────────
    The JSON is PER BONE — `tracks[bone] = [{frame, rot}, ...]` — because that is
    how an exporter walks a rig. `_bake` and tools/poser want PER FRAME —
    `keys = [{frame, ease, bones: {bone: rot}}, ...]` — because that is how an
    animator thinks about a pose. Same information, and the transpose is the
    whole difference.

    ── euler_order is carried, not assumed ─────────────────────────────────
    A real rig JSON has declared "YZX" for its source data.
    tools/poser/src/pose.js's convention is Blender pose-bone XYZ, and
    tools/poser/verify.py exists precisely because a wrong Euler order
    produces a viewport showing a plausible character doing plausible
    things that is not the one the ROM will contain. So the order travels
    with the data and a consumer that ignores it is choosing to.
    """
    rig = json.loads(Path(rig_path).read_text())
    order = rig.get("euler_order", "XYZ")

    out = []
    for anim in rig.get("anims", []):
        frames = sorted({k["frame"]
                         for track in anim.get("tracks", {}).values()
                         for k in track})
        keys = []
        for f in frames:
            bones = {}
            for bone, track in sorted(anim["tracks"].items()):
                for k in track:
                    if k["frame"] == f and "rot" in k:
                        bones[bone] = _round(k["rot"])
            # "linear" because these ARE the exporter's own keys: whatever easing
            # the generator applied is already resolved into them. Claiming an
            # easing mode here would invent one.
            keys.append({"frame": int(f), "ease": "linear", "bones": bones})
        out.append({"name": anim["name"], "length": int(anim["length"]),
                    "loop": bool(anim.get("loop", True)), "keys": keys,
                    "euler_order": order})
    return out


def capture(script, model, rig=None):
    """Run a model script's action builders with the bakers stubbed out.

    Returns [{name, length, loop, keys:[{frame, ease, bones}]}] in the order
    the script builds them.
    """
    import importlib.util

    import kilnlib as m

    captured = []

    def fake_bake(armature, name, keys, length, rest=None, loop=True, step=4):
        out = []
        for entry in keys:
            frame, pose = entry[0], entry[1]
            ease = entry[2] if len(entry) > 2 else "inout"
            out.append({"frame": int(frame), "ease": ease,
                        "bones": {b: _round(r) for b, r in sorted(pose.items())}})
        captured.append({"name": name, "length": int(length),
                         "loop": bool(loop), "keys": out})
        return None

    def fake_make_action(armature, name, channels, length):
        """The path anim_walk-style actions that call make_action directly
        take, and the one vehicles.py uses for every action it has."""
        frames = sorted({f for keys in channels.values() for f, _ in keys})
        out = []
        for f in frames:
            bones = {}
            for bone, keys in channels.items():
                for kf, val in keys:
                    if kf == f and 'rot' in val:
                        bones[bone] = _round(val['rot'])
            out.append({"frame": int(f), "ease": "linear",
                        "bones": dict(sorted(bones.items()))})
        captured.append({"name": name, "length": int(length),
                         "loop": True, "keys": out})
        return None

    m.make_armature = lambda name, bones: None
    m.make_skinned_mesh = lambda *a, **k: None
    m.make_mesh = lambda *a, **k: None
    m.reset_scene = lambda: None
    m.report = lambda max_tris=None: None
    m.export_gltf = lambda *a, **k: None
    m.make_action = fake_make_action

    spec = importlib.util.spec_from_file_location("m_" + model, HERE / script)
    mod = importlib.util.module_from_spec(spec)

    # `--rig` is for rig-JSON characters. Harmless to the others: kilnlib.arg()
    # just never looks it up.
    sys.argv = ["blender", "--", "--model", model, "--out", "/dev/null"]
    if rig:
        sys.argv += ["--rig", str(rig)]

    # `_bake` lives in the model module, so it can only be replaced after the
    # module object exists but before its main() runs — which is exactly what
    # exec_module does in one step. Patch it via the module's globals by
    # pre-seeding, then let main() run.
    spec.loader.exec_module(mod)

    if hasattr(mod, "_bake"):
        captured.clear()
        mod._bake = fake_bake
        m.make_action = fake_make_action
        if hasattr(mod, "build_all_actions"):
            mod.build_all_actions(None, model)
    return captured


def dump(script, model, out_dir, rig=None):
    actions = capture_rig(rig) if rig else capture(script, model)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    index = []
    for a in actions:
        path = out / f"{model}.{a['name']}.json"
        path.write_text(json.dumps(a, indent=1) + "\n")
        index.append({"name": a["name"], "length": a["length"],
                      "loop": a["loop"], "keys": len(a["keys"]),
                      "file": path.name})
        print(f"  {a['name']:<12} {a['length']:>3}f  {len(a['keys'])} keys "
              f"-> {path.name}")
    (out / f"{model}.index.json").write_text(
        json.dumps({"model": model, "script": script, "actions": index},
                   indent=1) + "\n")
    return actions


def load(path):
    """Read a JSON action back into `_bake`'s argument shape."""
    d = json.loads(Path(path).read_text())
    keys = []
    for k in d["keys"]:
        bones = {b: tuple(v) for b, v in k["bones"].items()}
        ease = k.get("ease", "inout")
        keys.append((k["frame"], bones) if ease == "linear"
                    else (k["frame"], bones, ease))
    return d["name"], keys, d["length"], d.get("loop", True)


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().split("\n\n")[1])
        return 2
    cmd, script, model = argv[0], argv[1], argv[2]
    if not script.endswith(".py"):
        script += ".py"
    # --rig <path> anywhere after the positionals, for the rig-JSON characters.
    rig = None
    if "--rig" in argv:
        rig = argv[argv.index("--rig") + 1]
        argv = [a for i, a in enumerate(argv)
                if i not in (argv.index("--rig"), argv.index("--rig") + 1)]
    if cmd == "list":
        for a in (capture_rig(rig) if rig else capture(script, model)):
            print(f"  {a['name']:<12} {a['length']:>3}f  "
                  f"{len(a['keys'])} keys  loop={a['loop']}")
        return 0
    if cmd == "dump":
        out_dir = argv[3] if len(argv) > 3 else "tools/poser/data"
        dump(script, model, out_dir, rig=rig)
        return 0
    print(f"anim_io: unknown command '{cmd}'")
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
