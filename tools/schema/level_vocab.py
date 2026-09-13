#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""level_vocab.py — the loader and the generator for tools/schema/level_vocab.json.

That JSON is the ONE statement of Kiln's level-content vocabulary: the entity
classnames, their epairs, the engine's capacities, and the canonical AABB face
winding. Before it, the same facts lived in six hand-maintained copies with
nothing comparing any pair:

    tools/mapmaker/src/entity.js   ENTITY_PALETTE      13 classnames
    tools/mapmaker/src/main.js     EPAIR_SCHEMAS        5 classnames, typed
    tools/mapmaker/validate.py     required_epairs      5 classnames + LIMITS
    tools/blender-mcp/server.py    a 14-way elif chain, a duplicate of the above
    Forge/src/forge_ent.c          CLASSNAMES           8, + a DISJOINT epair set
    engine/src/kiln/kiln_map.c     MAX_*                the same numbers again

They had drifted. forge_ent.c's comment claims it mirrors entity.js; it holds 8
of the 13 and a different epair vocabulary. examples/cinematic-demo registers
info_droid and info_alien, which entity.js had never heard of, so
assets/hangar.map could only be edited through a (custom) text prompt.

Python consumers import this module. JS and C consumers read a GENERATED file,
committed to the tree and diffed by nix/checks/level-vocab.nix — the same
regenerate-and-diff shape nix/checks/kiln-font.nix uses.

    python3 tools/schema/level_vocab.py --emit-js     tools/mapmaker/src/vocab.gen.js
    python3 tools/schema/level_vocab.py --emit-forge  Forge/src/forge_vocab.gen.h
    python3 tools/schema/level_vocab.py --emit-engine engine/src/kiln/kiln_levelvocab.h

profile_id appears nowhere and must never appear: kiln_map_register_classname
is a per-GAME call binding a classname to that game's own actor profile enum.
The schema owns the vocabulary; it does not own anyone's numbering.
"""

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
JSON_PATH = HERE / "level_vocab.json"

_cache = None
_cache_mtime = None


def load():
    """Re-reads when the JSON changes on disk.

    A plain module-global cache is wrong for the one consumer that is a
    long-lived PROCESS rather than a script: tools/blender-mcp/server.py runs
    against the live working tree by design, so an agent that edits the schema
    and then calls a tool would get the vocabulary as it was when the server
    started -- silently, with no indication the answer was stale. Every other
    consumer is a short-lived CLI where the mtime check costs one stat().
    """
    global _cache, _cache_mtime
    mtime = JSON_PATH.stat().st_mtime_ns
    if _cache is None or mtime != _cache_mtime:
        _cache = json.loads(JSON_PATH.read_text())
        _cache_mtime = mtime
    return _cache


def limits():
    """The engine's capacities. `faces` is DERIVED as brushes x 6, exactly as
    kiln_map.c derives MAX_FACES, so 256x6 is not written down twice."""
    lim = dict(load()["limits"])
    lim["faces"] = lim["brushes"] * 6
    return lim


def classnames():
    return [c["name"] for c in load()["classnames"]]


def palette():
    return {c["name"]: c for c in load()["classnames"]}


def category_of(name):
    return palette().get(name, {}).get("category", "other")


def required_epairs():
    return {c["name"]: [e["key"] for e in c.get("epairs", []) if e.get("required")]
            for c in load()["classnames"] if c.get("epairs")}


def forge_classnames():
    """In forge_index order. This is a WIRE FORMAT: the .FRG v2 tail stores
    `u8 classname` as an index into this list, so reordering it silently
    reinterprets every level already saved to an SD card."""
    got = [c for c in load()["classnames"] if "forge_index" in c]
    got.sort(key=lambda c: c["forge_index"])
    for i, c in enumerate(got):
        if c["forge_index"] != i:
            raise SystemExit(f"level_vocab: forge_index must be contiguous from 0; "
                             f"{c['name']} has {c['forge_index']}, expected {i}")
    return [c["name"] for c in got]


def forge_epair_slots():
    """How many numeric epair slots ENT mode gives an entity. A WIRE FORMAT:
    the .FRG v2 tail stores `u16 epair[slots]` positionally, so changing this
    number is an FRG_VERSION bump in Forge/src/forge_io.c and tools/forge/frg.py
    together, not a schema tweak."""
    return int(load()["forge_epair_slots"])


def forge_generic_epairs():
    """The untyped escape hatch -- count/delay/speed -- offered on any slot a
    classname does not claim with a declared numeric epair of its own. Nothing
    in the engine or the examples consumes them; they exist so a placement can
    carry a number at all."""
    return list(load()["forge_generic_epairs"])


def forge_epairs(classname):
    """The epair slots ENT mode offers for one classname, in slot order.

    Exactly forge_epair_slots() entries of {key, default, required}. The
    classname's own `numeric: true` epairs come FIRST, in declaration order --
    that is the fix for the defect this function exists for: Forge used to offer
    one global count/delay/speed to every classname, so info_key_door's required
    `key_id` was unreachable from the console and a level authored on hardware
    came back through ./dev forge-pull with a WARN nobody holding the controller
    could act on.

    A `numeric` epair is one a D-pad can author. `dialogue` (text), `mins`/`maxs`
    (vec3) and the `select` forms are deliberately NOT here: see forge_waived.
    """
    slots = forge_epair_slots()
    declared = [e for e in palette().get(classname, {}).get("epairs", [])
                if e.get("numeric")]
    if len(declared) > slots:
        raise SystemExit(
            f"level_vocab: {classname} declares {len(declared)} numeric epairs "
            f"but Forge has {slots} slots. Widening forge_epair_slots changes "
            f"the .FRG payload LAYOUT -- bump FRG_VERSION in "
            f"Forge/src/forge_io.c and tools/forge/frg.py together, and keep a "
            f"read path for the old one.")
    out = [{"key": e["key"], "default": int(e.get("default") or 0),
            "required": bool(e.get("required"))} for e in declared]
    taken = {e["key"] for e in out}
    for k in forge_generic_epairs():
        if len(out) >= slots:
            break
        if k not in taken:
            out.append({"key": k, "default": 0, "required": False})
    while len(out) < slots:          # a short generic list is not a wire change
        out.append({"key": "-", "default": 0, "required": False})
    return out


def aabb_face_table():
    return load()["aabb_faces"]


def aabb_faces(mn, mx):
    """The 6 canonical faces of an AABB, in the winding quake_map.py's CSG
    requires. Built from the table rather than transcribed."""
    sel = (mn, mx)
    return [tuple(tuple(sel[c[a]][a] for a in range(3)) for c in f["corners"])
            for f in aabb_face_table()]


# ── generators ──────────────────────────────────────────────────────────────

BANNER = ("GENERATED by tools/schema/level_vocab.py from "
          "tools/schema/level_vocab.json.\nDo not edit. "
          "nix/checks/level-vocab.nix regenerates this and diffs it.")


def emit_js():
    d = load()
    lim = limits()
    out = [f"// SPDX-License-Identifier: MIT", f"//", ]
    out += [f"// {l}" for l in BANNER.split("\n")]
    out += ["", "export const LIMITS = {"]
    for k in ("brushes", "faces", "spawns", "classnames", "coord"):
        out.append(f"  {k}: {lim[k]},")
    out += ["};", "",
            "// size = arrow length or box half-extent, in world units.",
            "// shape: 'arrow' | 'box' | 'cylinder' | 'diamond' | 'wireframe'",
            "export const ENTITY_PALETTE = {"]
    w = max(len(c["name"]) for c in d["classnames"]) + 1
    for c in d["classnames"]:
        out.append(f"  {(c['name'] + ':').ljust(w)} {{ color: {c['color']}, "
                   f"size: {c['size']}, shape: '{c['shape']}', "
                   f"label: '{c['label']}' }},")
    out += ["};", "", "export const KNOWN_CLASSNAMES = Object.keys(ENTITY_PALETTE);",
            "", "// Typed epair forms. 'generic' epairs remain available below these.",
            "export const EPAIR_SCHEMAS = {"]
    for c in d["classnames"]:
        eps = c.get("epairs") or []
        if not eps:
            continue
        out.append(f"  {c['name']}: [")
        for e in eps:
            bits = [f"key: '{e['key']}'", f"label: '{e['label']}'",
                    f"type: '{e['type']}'"]
            if "options" in e:
                opts = ", ".join("{ value: '%s', label: '%s' }" % (o["value"], o["label"])
                                 for o in e["options"])
                bits.append(f"options: [{opts}]")
            dv = e["default"]
            bits.append(f"default: {dv}" if isinstance(dv, (int, float))
                        else f"default: '{dv}'")
            out.append("    { " + ", ".join(bits) + " },")
        out.append("  ],")
    out += ["};", "",
            "// The canonical 6-face corner table, as data. 0 = mins, 1 = maxs, per axis.",
            "export const AABB_FACE_CORNERS = ["]
    for f in d["aabb_faces"]:
        out.append(f"  {json.dumps(f['corners'])}, // {f['name']}")
    out += ["];", "",
            "export const AABB_FACE_NAMES = "
            + json.dumps([f["name"] for f in d["aabb_faces"]]) + ";", ""]
    return "\n".join(out)


def emit_forge():
    d = load()
    cls = forge_classnames()
    slots = forge_epair_slots()
    eps = [forge_epairs(c) for c in cls]
    out = ["/* SPDX-License-Identifier: MIT", " *"]
    out += [" * " + l for l in BANNER.split("\n")]
    out += [" *",
            " * The classname ORDER here is a wire format: the .FRG v2 tail stores",
            " * `u8 classname` as an index into it (Forge/src/forge_io.c, and",
            " * tools/forge/frg.py). Reordering silently reinterprets every level",
            " * already saved to an SD card. Append, or bump FRG_VERSION on both sides.",
            " *",
            " * The EPAIR SLOT COUNT is a wire format too -- the same tail stores",
            " * `u16 epair[FORGE_VOCAB_EPAIR_COUNT]` positionally. The KEY of a slot",
            " * depends on the classname (that classname's own numeric epairs first,",
            " * then the generic count/delay/speed), so the tables below are indexed",
            " * by classname x slot rather than by slot alone. That",
            " * is what makes info_key_door's required `key_id` reachable from a",
            " * controller at all; it used to be three global keys for every class.",
            " *",
            " * Tables live inside `static inline` accessors rather than at file scope:",
            " * a file-scope `static const` table in a header is unused in every TU that",
            " * does not touch it, and this tree compiles at -Werror under both gcc and",
            " * emcc.",
            " */",
            "#ifndef FORGE_VOCAB_GEN_H", "#define FORGE_VOCAB_GEN_H", "",
            f"#define FORGE_VOCAB_CLASSNAME_COUNT {len(cls)}",
            f"#define FORGE_VOCAB_EPAIR_COUNT     {slots}",
            f"#define FORGE_VOCAB_FACE_COUNT      {len(d['aabb_faces'])}", ""]
    out += ["static inline const char *forge_vocab_classname(int i)", "{",
            "    static const char *const T[FORGE_VOCAB_CLASSNAME_COUNT] = {"]
    out += [f'        "{c}",' for c in cls]
    out += ["    };",
            "    return T[(i % FORGE_VOCAB_CLASSNAME_COUNT "
            "+ FORGE_VOCAB_CLASSNAME_COUNT) % FORGE_VOCAB_CLASSNAME_COUNT];",
            "}", ""]
    out += ["/* `c` is a classname index, `i` a slot 0..FORGE_VOCAB_EPAIR_COUNT-1.",
            " * Both are wrapped rather than asserted: this header is included by",
            " * forge_io.c's LOADER, which reads both out of a file on an SD card.",
            " */",
            "static inline int forge_vocab_epair_slot(int c, int i)", "{",
            "    c = (c % FORGE_VOCAB_CLASSNAME_COUNT "
            "+ FORGE_VOCAB_CLASSNAME_COUNT) % FORGE_VOCAB_CLASSNAME_COUNT;",
            "    i = (i % FORGE_VOCAB_EPAIR_COUNT "
            "+ FORGE_VOCAB_EPAIR_COUNT) % FORGE_VOCAB_EPAIR_COUNT;",
            "    return c * FORGE_VOCAB_EPAIR_COUNT + i;", "}", ""]

    def _flat(cfmt):
        rows = []
        for c, row in zip(cls, eps):
            rows.append("        " + " ".join(cfmt(e) for e in row)
                        + f"  /* {c} */")
        return rows

    out += ["static inline const char *forge_vocab_epair_key(int c, int i)", "{",
            "    static const char *const T[FORGE_VOCAB_CLASSNAME_COUNT "
            "* FORGE_VOCAB_EPAIR_COUNT] = {"]
    out += _flat(lambda e: '"%s",' % e["key"])
    out += ["    };", "    return T[forge_vocab_epair_slot(c, i)];", "}", "",
            "/* The value a freshly placed entity starts that slot at. A required",
            " * epair seeded to 0 would be emitted as an authored 0 or omitted",
            " * entirely -- both of which are the WARN this table exists to end. */",
            "static inline int forge_vocab_epair_default(int c, int i)", "{",
            "    static const short T[FORGE_VOCAB_CLASSNAME_COUNT "
            "* FORGE_VOCAB_EPAIR_COUNT] = {"]
    out += _flat(lambda e: "%d," % e["default"])
    out += ["    };", "    return T[forge_vocab_epair_slot(c, i)];", "}", "",
            "/* Required epairs are written to the .map even at 0: the validator",
            " * warns on ABSENCE, and an author who never touched the field is",
            " * exactly the case that warning was firing on. */",
            "static inline int forge_vocab_epair_required(int c, int i)", "{",
            "    static const unsigned char T[FORGE_VOCAB_CLASSNAME_COUNT "
            "* FORGE_VOCAB_EPAIR_COUNT] = {"]
    out += _flat(lambda e: "%d," % int(e["required"]))
    out += ["    };", "    return T[forge_vocab_epair_slot(c, i)];", "}", ""]
    out += ["static inline const char *forge_vocab_face_name(int f)", "{",
            "    static const char *const T[FORGE_VOCAB_FACE_COUNT] = {"]
    out += [f'        "{f["name"]}",' for f in d["aabb_faces"]]
    out += ["    };", "    return T[f];", "}", "",
            "/* Corner `pt` (0..2) of face `f`, selecting per axis from mn/mx.",
            " * A loop over the GENERATED selector, not over axes -- which is what",
            " * Forge/src/forge_io.c's standing warning about re-deriving the winding",
            " * by hand was protecting. */",
            "static inline void forge_vocab_aabb_corner(int f, int pt,",
            "                                           const int mn[3],",
            "                                           const int mx[3], int out[3])",
            "{",
            "    static const unsigned char SEL"
            "[FORGE_VOCAB_FACE_COUNT][3][3] = {"]
    for face in d["aabb_faces"]:
        rows = ", ".join("{ %d, %d, %d }" % tuple(c) for c in face["corners"])
        out.append(f'        {{ {rows} }},  /* {face["name"]} */')
    out += ["    };", "    for (int a = 0; a < 3; a++)",
            "        out[a] = SEL[f][pt][a] ? mx[a] : mn[a];", "}", "",
            "#endif /* FORGE_VOCAB_GEN_H */", ""]
    return "\n".join(out)


def emit_engine():
    lim = limits()
    out = ["/* SPDX-License-Identifier: MIT", " *"]
    out += [" * " + l for l in BANNER.split("\n")]
    out += [" *",
            " * Macros only, deliberately. The engine has no use for the classname",
            " * strings -- kiln_map_register_classname is how a GAME binds one to its",
            " * own profile id -- and a header that published both a KILN_LEVEL_* macro",
            " * set and a table would be two namespaces in one file. See CLAUDE.md:",
            ' * "a generated header is a namespace, not just a file".',
            " *",
            " * KILN_LEVEL_MAX_FACES is brushes x 6, the way kiln_map.c always derived",
            " * MAX_FACES; storing 1536 would be the same arithmetic written twice.",
            " */",
            "#ifndef KILN_LEVELVOCAB_H", "#define KILN_LEVELVOCAB_H", "",
            f"#define KILN_LEVEL_MAX_BRUSHES    {lim['brushes']}",
            f"#define KILN_LEVEL_MAX_FACES      (KILN_LEVEL_MAX_BRUSHES * 6)",
            f"#define KILN_LEVEL_MAX_SPAWNS     {lim['spawns']}",
            f"#define KILN_LEVEL_MAX_CLASSNAMES {lim['classnames']}",
            f"#define KILN_LEVEL_MAX_ENTITIES   {lim['entities']}",
            f"#define KILN_LEVEL_MAX_COORD      {lim['coord']}",
            f"#define KILN_LEVEL_MAX_BRUSH_PLANES {lim['brush_planes']}",
            f"#define KILN_LEVEL_MAX_FACE_VERTS   {lim['face_verts']}", "",
            "#endif /* KILN_LEVELVOCAB_H */", ""]
    return "\n".join(out)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="level_vocab.py", description=__doc__.split("\n")[0])
    ap.add_argument("--emit-js", metavar="PATH")
    ap.add_argument("--emit-forge", metavar="PATH")
    ap.add_argument("--emit-engine", metavar="PATH")
    ap.add_argument("--stdout", action="store_true",
                    help="write to stdout instead of the given paths")
    a = ap.parse_args(argv[1:])
    jobs = [(a.emit_js, emit_js), (a.emit_forge, emit_forge),
            (a.emit_engine, emit_engine)]
    if not any(p for p, _ in jobs):
        ap.error("nothing to emit; pass --emit-js / --emit-forge / --emit-engine")
    for path, fn in jobs:
        if not path:
            continue
        text = fn()
        if a.stdout:
            sys.stdout.write(text)
        else:
            Path(path).write_text(text)
            print(f"wrote {path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
