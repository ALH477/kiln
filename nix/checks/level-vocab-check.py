#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Phase B of nix/checks/level-vocab.nix: the assertions a diff cannot make.

A diff proves the generated artifacts match the JSON. It cannot prove the JSON
is right, and it cannot see a CORRECT table used INCORRECTLY -- which is the
failure this repo actually had, six times over, in its own assets/ directory.
"""
import argparse
import re
import sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--repo", required=True)
ap.add_argument("--c-brush", required=True)
ap.add_argument("--examples", required=True)
ap.add_argument("--kiln-map", required=True)
a = ap.parse_args()

repo = Path(a.repo)
sys.path[:0] = [str(repo / "tools" / "schema"), str(repo / "tools" / "mapmaker"),
                str(repo / "tools" / "blender"), str(repo / "tools" / "forge")]
import level_vocab   # noqa: E402
import mapfmt        # noqa: E402
import quake_map     # noqa: E402

fails = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


# ── B1: the winding MEANS what the table says ──────────────────────────
# axis and sign are redundant with corners ON PURPOSE: they are what lets this
# assert the meaning rather than re-checking the transcription. A table that is
# corrupted but regenerated consistently passes Phase A and fails here.
print("B1 the winding means what it says")
MN, MX = [-64.0, -32.0, -64.0], [64.0, 32.0, 64.0]
for face, tri in zip(level_vocab.aabb_face_table(), level_vocab.aabb_faces(MN, MX)):
    p1, p2, p3 = tri
    u = [p3[i] - p1[i] for i in range(3)]
    v = [p2[i] - p1[i] for i in range(3)]
    n = [u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0]]
    mag = max(abs(c) for c in n)
    ax, sg = face["axis"], face["sign"]
    check(mag > 0, f"{face['name']}: cross(p3-p1, p2-p1) is non-zero")
    zeros = sum(1 for i in range(3) if abs(n[i]) < 1e-9)
    check(zeros == 2, f"{face['name']}: the normal is axis-aligned")
    check(abs(n[ax]) == mag and (n[ax] > 0) == (sg > 0),
          f"{face['name']}: the normal points {'+' if sg > 0 else '-'}"
          f"{'xyz'[ax]}, as declared")

# ── B2: four emitters, one brush, one answer ───────────────────────────
# The half that catches a correct table used incorrectly -- e.g. two swapped
# snprintf arguments in Forge, which no diff of a data file can see.
print("B2 every emitter writes the same brush")
py = mapfmt.emit_state({"brushes": [{"mins": MN, "maxs": MX, "texture": "TEX"}],
                        "spawns": []})
py_brush = "\n".join(py.split("\n")[2:10])          # the { ... } block
c_brush = Path(a.c_brush).read_text().strip()
check(py_brush.strip() == c_brush,
      "Forge's C emitter and mapfmt.py agree byte for byte")
if py_brush.strip() != c_brush:
    print("    python:\n" + "\n".join("      " + l for l in py_brush.splitlines()))
    print("    C:\n" + "\n".join("      " + l for l in c_brush.splitlines()))

import frg  # noqa: E402
frg_brush = frg.boxes_to_map([(tuple(int(v) for v in MN),
                               tuple(int(v) for v in MX), 0)])
frg_block = "\n".join(frg_brush.split("\n")[2:10])
check(frg_block.strip().replace("FORGE0", "TEX") == c_brush,
      "tools/forge/frg.py agrees with them")

surviving = sum(len(v) for v in
                quake_map.brush_to_faces(quake_map.parse_map(py)[0]["brushes"][0]).values())
check(surviving == 6,
      f"the shared brush survives quake_map.py's CSG with 6 faces (got {surviving})")

# ── B3: every registered classname is in the vocabulary ────────────────
print("B3 every registered classname is in the vocabulary")
known = set(level_vocab.classnames())
registered = set()
for c in Path(a.examples).rglob("*.c"):
    registered |= set(re.findall(r'kiln_map_register_classname\("([a-z_0-9]+)"',
                                 c.read_text()))
for cn in sorted(registered):
    check(cn in known,
          f"{cn} is registered by an example and known to the schema")
check(len(known) <= level_vocab.limits()["classnames"],
      f"{len(known)} classnames fits MAX_CLASSNAMES "
      f"({level_vocab.limits()['classnames']})")

# ── B4: the engine did not re-hardcode its limits ──────────────────────
print("B4 the engine's limits come from the generated header")
src = Path(a.kiln_map).read_text()
for m in re.finditer(r'#define\s+(MAX_(?:BRUSHES|FACES|SPAWNS|CLASSNAMES|ENTITIES))\s+(.+)',
                     src):
    name, val = m.group(1), m.group(2).strip()
    check("KILN_LEVEL_" in val,
          f"kiln_map.c's {name} comes from the generated header "
          f"(expands to {val!r})")

# ── B5: internal consistency ───────────────────────────────────────────
print("B5 the schema is internally consistent")
level_vocab.forge_classnames()   # raises if forge_index is not contiguous from 0
check(True, "forge_index is contiguous from 0 (a wire format)")
pal = level_vocab.palette()
for c in level_vocab.load()["classnames"]:
    for e in c.get("epairs", []):
        if e["type"] == "door_select":
            check(e.get("target_classname") in pal,
                  f"{c['name']}.{e['key']} targets a classname that exists")
        check("default" in e, f"{c['name']}.{e['key']} has a default")

# ── B6: Forge can author what the validator requires ───────────────────
# The defect: ENT mode offered one GLOBAL count/delay/speed to every classname,
# while mapfmt.analyse warns on a missing `key_id` for info_key_door. Neither
# key was reachable from the console, so a level authored on hardware came back
# through ./dev forge-pull carrying a warning nobody holding the controller
# could act on.
#
# This used to filter the requirement down to `numeric` epairs, which quietly
# excused every string one -- so the info_npc waiver could never fire and said
# nothing. It now asks the honest question (is EVERY required epair of a
# Forge-authorable classname reachable in ENT mode?) and a waiver has to carry
# the real answer for the ones that are not.
print("B6 Forge can author what the validator requires")
waived = level_vocab.load().get("forge_waived", {})
for name in level_vocab.forge_classnames():
    slots = level_vocab.forge_epairs(name)
    check(len(slots) == level_vocab.forge_epair_slots(),
          f"{name} has {level_vocab.forge_epair_slots()} epair slots "
          f"(the .FRG v2 tail stores exactly that many, positionally)")
    keys = {s["key"] for s in slots}
    missing = [e["key"] for e in pal[name].get("epairs", [])
               if e.get("required") and e["key"] not in keys]
    if missing and name in waived:
        print(f"  waived {name}: {waived[name]}")
        continue
    check(not missing,
          f"{name}'s required epairs {missing} are authorable in ENT mode")

# Reachable is not enough. The warning fires on ABSENCE from the .map, so a
# required slot an author never opened must still be WRITTEN -- which is why
# forge_ent_emit's "omit a zero" rule has an exception and frg.py mirrors it.
# Asserted end to end through the emitter rather than by reading the table,
# because a correct table used incorrectly is the failure this whole phase is
# for.
print("   and reach the .map even when the author never opened the field")
for i, name in enumerate(level_vocab.forge_classnames()):
    req = [s["key"] for s in level_vocab.forge_epairs(name) if s["required"]]
    if not req:
        continue
    text = frg.boxes_to_map([], ents=[{"pos": (0.0, 0.0, 0.0), "angle": 0,
                                       "classname": i,
                                       "epairs": [0] * level_vocab.forge_epair_slots()}],
                            classnames=level_vocab.forge_classnames())
    for k in req:
        check(f'"{k}"' in text,
              f"a freshly placed {name} emits {k} without being touched")
    props = {kv[0]: kv[1] for kv in
             re.findall(r'"([a-z_0-9]+)" "([^"]*)"', text)}
    for k in req:
        check(k in props, f"and mapfmt/validate.py sees {name}.{k} as present")

# A waiver must name a classname that exists and actually needs one; a stale
# one is worse than none, because it reads as a known limitation somebody is
# tracking. info_key_door's waiver was deleted here when ENT mode grew per-
# classname keys; info_npc's remains because `dialogue` is a string and a
# u16 slot cannot carry one -- a numeric stand-in would satisfy the validator
# and hand the game nothing to read, which is worse than the warning.
print("   and no waiver outlives its defect")
for wname, why in waived.items():
    check(wname in pal, f"waived {wname} is a real classname")
    check(bool(why and why.strip()), f"the {wname} waiver says why")
    if wname in pal:
        wkeys = ({s["key"] for s in level_vocab.forge_epairs(wname)}
                 if wname in level_vocab.forge_classnames() else set())
        still = [e["key"] for e in pal[wname].get("epairs", [])
                 if e.get("required") and e["key"] not in wkeys]
        check(bool(still),
              f"the {wname} waiver is still load-bearing ({still}); a waiver "
              f"for something already fixed should be deleted")

print()
if fails:
    print(f"FAILED ({len(fails)})")
    sys.exit(1)
print("the level vocabulary has one statement, and it means what it says")
