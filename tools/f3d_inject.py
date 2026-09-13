#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""f3d_inject.py — write `extras.f3d_mat` into a glTF's materials.

── Why this exists ────────────────────────────────────────────────────────
Tiny3D's importer only understands Fast64 materials. Fast64 is a large
third-party Blender addon that tracks Blender 4.2-4.5; this flake's nixpkgs
ships Blender 5.2. Putting the ROM build on the wrong side of that version race
buys nothing, because the thing gltf_to_t3d actually consumes is not the addon
— it is a JSON object sitting in `materials[i].extras.f3d_mat`, which Fast64
happens to be one way of producing. This script is the other way.

So: Blender authors geometry, this writes the material block.

── The schema ─────────────────────────────────────────────────────────────
Everything below was read out of Tiny3D's
`tools/gltf_importer/src/parser/materialParser.cpp`. Keys the importer reads
with `operator[]` rather than `.contains()`/`.value()` are MANDATORY — a
missing one is a null that throws on `.get<>()`, not a default. Those are:

    f3d_mat.combiner1, f3d_mat.combiner2                      (:218-219)
    and, once `rdp_settings` is present at all (:241-250):
      g_mdsft_cycletype, g_cull_back, g_cull_front, g_fog,
      g_mdsft_text_filt, g_tex_gen, set_rendermode
    with set_rendermode != 0 -> rendermode_preset_cycle_1/2   (:275-277)
    with set_rendermode == 0 -> draw_layer.{oot,sm64}         (:295-296)
    and inside tex0/tex1, S/T -> high, low, mask, shift       (:29-38)

Optional (guarded by `.contains`): set_prim/prim_color, set_env/env_color,
set_blend/blend_color, tex0, tex1, use_tex_reference, clamp, mirror.

Colours are float[4] in LINEAR space; the importer applies pow(c, 1/2.2)
(:145-159), same as it does to COLOR_0 vertex colours.

── Combiner operand values are raw RDP encodings ──────────────────────────
readCCFromJson (:119-135) reads A..D as plain integers and shifts them straight
into the RDP combiner word — it does not translate. So these must match
libdragon's `_RDPQ_COMB1_*` tables in `include/rdpq_macros.h`, NOT some
Fast64-internal enum. The per-field zero encoding differs by field (RGB suba 8,
RGB subb 8, RGB mul 16, RGB add 7, alpha addsub 7, alpha mul 7), which is why
the tables below are spelled out per field instead of sharing one constant.

Two derived flags fall out of the combiner and are worth knowing about:
  * TEXTURED is inferred from the combiner naming TEX0/TEX1 (isCCUsingTexture,
    :139-149) — and only then are tex0/tex1 read at all.
  * SHADED is inferred from it naming SHADE (isUsingShade, :152-157).
There is no way to ask for either directly; you ask by picking a combiner.

Usage:
  f3d_inject.py <in.gltf> <out.gltf> --material NAME=PRESET[,k=v]...

  e.g. --material Checker=tex0_shade,tex=textures/checker.i8.png,size=32
       --material Water=tex0_alpha,tex=textures/water.ia8.png,size=32,mode=transparent
       --material GoblinSkin=shade
       --material Fire=tex0_shade,useRef=1,refAddress=0x01,refSize=32:32

  --material '*=shade' applies a preset to every material not named explicitly.

  useRef=1 marks the texture as a dynamic reference (flipbook/offscreen).
  The model loads no texture at conversion time; at runtime the engine
  swaps which sprite is bound via rdpq_set_lookup_address(refAddress).
  Requires a textured preset (tex0_shade / tex0_alpha).
"""

import json
import sys
from pathlib import Path

# ── RDP colour-combiner operand encodings (libdragon rdpq_macros.h) ────────
# Named per field because the encoding of "zero" is not the same number in all
# of them. Getting this wrong produces a model that renders, in the wrong
# colour, with no diagnostic anywhere — hence the explicit tables.
RGB_SUBA = {"COMBINED": 0, "TEX0": 1, "TEX1": 2, "PRIM": 3, "SHADE": 4,
            "ENV": 5, "ONE": 6, "NOISE": 7, "ZERO": 8}
RGB_SUBB = {"COMBINED": 0, "TEX0": 1, "TEX1": 2, "PRIM": 3, "SHADE": 4,
            "ENV": 5, "KEYCENTER": 6, "K4": 7, "ZERO": 8}
RGB_MUL = {"COMBINED": 0, "TEX0": 1, "TEX1": 2, "PRIM": 3, "SHADE": 4,
           "ENV": 5, "KEYSCALE": 6, "COMBINED_ALPHA": 7, "TEX0_ALPHA": 8,
           "TEX1_ALPHA": 9, "PRIM_ALPHA": 10, "SHADE_ALPHA": 11,
           "ENV_ALPHA": 12, "LOD_FRAC": 13, "PRIM_LOD_FRAC": 14, "K5": 15,
           "ZERO": 16}
RGB_ADD = {"COMBINED": 0, "TEX0": 1, "TEX1": 2, "PRIM": 3, "SHADE": 4,
           "ENV": 5, "ONE": 6, "ZERO": 7}
A_ADDSUB = {"COMBINED": 0, "TEX0": 1, "TEX1": 2, "PRIM": 3, "SHADE": 4,
            "ENV": 5, "ONE": 6, "ZERO": 7}
A_MUL = {"LOD_FRAC": 0, "TEX0": 1, "TEX1": 2, "PRIM": 3, "SHADE": 4,
         "ENV": 5, "PRIM_LOD_FRAC": 6, "ZERO": 7}


def combiner(rgb, alpha):
    """(A,B,C,D) x2, given symbolically, encoded per field."""
    ra, rb, rc, rd = rgb
    aa, ab, ac, ad = alpha
    return {
        "A": RGB_SUBA[ra], "B": RGB_SUBB[rb], "C": RGB_MUL[rc], "D": RGB_ADD[rd],
        "A_alpha": A_ADDSUB[aa], "B_alpha": A_ADDSUB[ab],
        "C_alpha": A_MUL[ac], "D_alpha": A_ADDSUB[ad],
    }


# ── Presets ────────────────────────────────────────────────────────────────
# `shade` reproduces libdragon's RDPQ_COMBINER_SHADE exactly — the combiner
# kiln_scene_begin() already sets and that examples/engine already renders with.
# Matching it means an untextured model looks identical whether it went through
# --ignore-materials (which leaves the scene's combiner alone) or through here.
PRESETS = {
    # vertex colour x directional lighting, no texture, no TMEM
    "shade": dict(
        combiner1=combiner(("ZERO", "ZERO", "ZERO", "SHADE"),
                           ("ZERO", "ZERO", "ZERO", "SHADE")),
        textured=False,
    ),
    # texture modulated by the lit vertex colour; opaque
    "tex0_shade": dict(
        combiner1=combiner(("TEX0", "ZERO", "SHADE", "ZERO"),
                           ("ZERO", "ZERO", "ZERO", "ONE")),
        textured=True,
    ),
    # as above but alpha comes from the texture — for IA/RGBA textures whose
    # alpha channel is doing real work (water, foliage cutouts)
    "tex0_alpha": dict(
        combiner1=combiner(("TEX0", "ZERO", "SHADE", "ZERO"),
                           ("ZERO", "ZERO", "ZERO", "TEX0")),
        textured=True,
    ),
    # flat primitive colour, no lighting — for anything that must not be shaded
    "prim": dict(
        combiner1=combiner(("ZERO", "ZERO", "ZERO", "PRIM"),
                           ("ZERO", "ZERO", "ZERO", "PRIM")),
        textured=False,
    ),
    # ── A decal combiner for a palette-swap material ────────────────────
    # The texture STRAIGHT THROUGH — no shade multiply. This is fast64's
    # G_CC_DECALRGBA, and for a material built on the n64-modeling skill's
    # "CI4 and a palette-swap contract" it is a requirement rather than a
    # style, measured the hard way:
    #
    #   "A material meant to own the bright end of a value-rationed palette
    #    must draw near-DECAL, not modulated. Run it through the same
    #    modulate combiner as everything else and it comes out DARKER than
    #    its surroundings — the exact inverse of the intent. Vertex colour x
    #    lighting x TLUT is three multiplications and the bright end of the
    #    palette never survives it."
    #
    # The whole point of that contract is that some material class owns true
    # black and true white while another is rationed to a mid band. Multiplying
    # its texel by a shade term below 1 gives that contrast away, and no
    # palette entry can win it back. It is a combiner mode, not an extra pass,
    # so it costs nothing.
    #
    # Alpha comes from the texture, because a CI4 veil-style palette carries
    # its transparency in the RGBA5551 alpha bit — that bit is what makes a
    # `phantom`-class material's cold state invisible.
    #
    # ── Why TEX0 * PRIM and not the literal (0,0,0,TEX0) ────────────────
    # fast64's G_CC_DECALRGBA is (0, 0, 0, TEXEL0) — TEX0 in the combiner's D
    # (add) slot. Written that way, this preset produced a model that converted
    # cleanly, shipped, and rendered with NO TEXTURE AT ALL: Tiny3D's material
    # parser gates whether it even reads the `tex0` block on whether the colour
    # combiner uses a texture (`isCCUsingTexture`, the same gate f3d_inject's
    # own validation below leans on), and TEX0 in the D slot alone does not
    # satisfy it. Nothing warns. The texture is never loaded, no TLUT is ever
    # uploaded, and the material falls back to its vertex colours — which on a
    # dark model is entirely plausible-looking.
    #
    # (TEX0 - ZERO) * PRIM + ZERO with PRIM white is the SAME arithmetic with
    # TEX0 in the A slot, where the gate sees it. RGB_MUL has no ONE operand, so
    # PRIM is how you spell "multiply by one" here — which is why this preset
    # requires `prim=1:1:1:1` and validates for it.
    "tex0_decal": dict(
        combiner1=combiner(("TEX0", "ZERO", "PRIM", "ZERO"),
                           ("TEX0", "ZERO", "PRIM", "ZERO")),
        textured=True,
        needs_prim=True,
    ),
}

# rendermode_preset_cycle_1 indices into fast64Types.h's F64_RENDER_MODE_1_*.
# Only these four are worth naming; the tables have 20 entries but most differ
# only in AA and z-buffer details that rdpq can set on top.
RENDER_MODES = {
    "opaque": 1,
    "decal": 2,        # ZMODE_DECAL — coplanar overlays, no z-fighting
    "cutout": 4,       # ALPHA_COMPARE — 1-bit alpha, still opaque blending
    "transparent": 5,  # RDP::BLEND::MULTIPLY
}

# g_mdsft_text_filt is indexed into a THREE-entry table (materialParser.cpp
# :261-267), so Fast64's G_TF_AVERAGE (3) would read off the end. Only expose
# the two that are in range.
TEX_FILTERS = {"point": 0, "bilerp": 2}


def tile_axis(size, clamp=False, mirror=False, shift=0):
    """S or T tile params. `mask` is log2(size): it is what makes the RDP wrap
    at the tile edge, so a non-power-of-two texture cannot repeat.

    high/low also seed `texWidth`/`texHeight` (high - low + 1) before the PNG
    is decoded, so they must describe the real texture even when a texture is
    present."""
    mask = max(0, (size - 1).bit_length()) if size > 1 else 0
    if (1 << mask) != size:
        raise ValueError(f"texture size {size} is not a power of two; the RDP "
                         f"cannot wrap it and mask would be meaningless")
    return {"low": 0.0, "high": float(size - 1), "mask": mask,
            "shift": shift, "clamp": 1 if clamp else 0,
            "mirror": 1 if mirror else 0}


def build_f3d_mat(preset, tex=None, size=32, mode="opaque", filt="bilerp",
                  cull="back", fog=False, clamp=False, mirror=False,
                  prim=None, env=None, blend=None,
                  useRef=False, refAddress="0x01", refSize=None):
    if preset not in PRESETS:
        raise SystemExit(f"f3d_inject: unknown preset '{preset}'; "
                         f"have {', '.join(sorted(PRESETS))}")
    spec = PRESETS[preset]

    if spec["textured"] and not tex and not useRef:
        raise SystemExit(f"f3d_inject: preset '{preset}' names TEX0 in its "
                          f"combiner, so it needs tex=<path/to/x.png> or "
                          f"useRef=1")
    if tex and not spec["textured"]:
        raise SystemExit(f"f3d_inject: preset '{preset}' has no TEX0 in its "
                          f"combiner, so tex= would be silently ignored "
                          f"(isCCUsingTexture gates whether tex0 is read at all)")
    if useRef and not spec["textured"]:
        raise SystemExit(f"f3d_inject: useRef=1 requires a textured preset "
                          f"(one with TEX0 in its combiner)")
    # A preset that spells "multiply by one" as PRIM is broken without one: an
    # undefined PRIM multiplies the texel by whatever colour the last draw
    # happened to leave in the register, so the material renders a different
    # wrong colour depending on what preceded it.
    if spec.get("needs_prim") and prim is None:
        raise SystemExit(f"f3d_inject: preset '{preset}' multiplies TEX0 by "
                          f"PRIM (RGB_MUL has no ONE operand), so it needs an "
                          f"explicit prim= — normally prim=1:1:1:1")

    mat = {
        "combiner1": spec["combiner1"],
        # 1-cycle mode, so combiner2 is never consulted for the final word —
        # but readCCFromJson dereferences it unconditionally, so it must exist.
        "combiner2": spec["combiner1"],
        "rdp_settings": {
            "g_mdsft_cycletype": 0,          # 1-cycle
            "g_cull_back": 1 if cull in ("back", "both") else 0,
            "g_cull_front": 1 if cull in ("front", "both") else 0,
            "g_fog": 1 if fog else 0,
            "g_mdsft_text_filt": TEX_FILTERS[filt],
            "g_tex_gen": 0,                  # no spherical UV generation
            "set_rendermode": 1,
            "rendermode_preset_cycle_1": RENDER_MODES[mode],
            "rendermode_preset_cycle_2": RENDER_MODES[mode],
        },
        # Only read when set_rendermode is 0. Carried anyway so the block stays
        # valid if someone flips that later.
        "draw_layer": {"oot": 0, "sm64": 0},
    }

    for key, value in (("prim", prim), ("env", env), ("blend", blend)):
        if value is not None:
            mat[f"set_{key}"] = 1
            mat[f"{key}_color"] = list(value)

    if useRef:
        # Tiny3D reads a texReference of 0 as "no texture" (t3dmodel.c), and
        # rdpq's lookup slots are 1..15, so 0 or anything past 15 names nothing.
        try:
            ref = int(str(refAddress), 0)
        except ValueError:
            raise SystemExit(f"f3d_inject: refAddress {refAddress!r} is not a number")
        if not 1 <= ref <= 15:
            raise SystemExit(f"f3d_inject: refAddress {refAddress!r} must be 1..15; "
                             f"0 is Tiny3D's 'no texture'")
        # gltf_to_t3d bakes the UVs against the reference size (parser.cpp),
        # so a missing one converts cleanly and samples the wrong texels.
        if not refSize:
            raise SystemExit("f3d_inject: useRef=1 needs refSize=W:H, the size of the "
                             "surface the game uploads — the UVs are baked against it")
        tex0 = {
            "use_tex_reference": 1,
            "tex_reference": refAddress,
            "S": tile_axis(size, clamp=clamp, mirror=mirror),
            "T": tile_axis(size, clamp=clamp, mirror=mirror),
        }
        if refSize:
            tex0["tex_reference_size"] = list(refSize)
        mat["tex0"] = tex0
    elif tex:
        mat["tex0"] = {
            "use_tex_reference": 0,
            # Resolved relative to the .gltf's own directory, then made
            # relative to the PROCESS CWD and rewritten to rom:/ if it falls
            # under --asset-path (materialParser.cpp mapRomPath). The build
            # has to run gltf_to_t3d from the staging root for that to work.
            "tex": {"name": tex},
            "S": tile_axis(size, clamp=clamp, mirror=mirror),
            "T": tile_axis(size, clamp=clamp, mirror=mirror),
        }

    return mat


def parse_material_arg(arg):
    """NAME=PRESET[,k=v]... -> (name, kwargs)"""
    if "=" not in arg:
        raise SystemExit(f"f3d_inject: --material wants NAME=PRESET[,k=v], got {arg!r}")
    name, _, rest = arg.partition("=")
    parts = rest.split(",")
    kwargs = {"preset": parts[0]}
    for part in parts[1:]:
        if not part:
            continue
        if "=" not in part:
            raise SystemExit(f"f3d_inject: option {part!r} in {arg!r} needs k=v")
        key, _, val = part.partition("=")
        if key == "size":
            kwargs[key] = int(val)
        elif key in ("clamp", "mirror", "fog", "useRef"):
            kwargs[key] = val.lower() in ("1", "true", "yes")
        elif key in ("prim", "env", "blend"):
            kwargs[key] = [float(c) for c in val.split(":")]
        elif key == "refSize":
            parts = val.split(":")
            kwargs[key] = [int(p) for p in parts]
        else:
            kwargs[key] = val
    return name, kwargs


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2

    in_path, out_path = Path(argv[1]), Path(argv[2])
    specs = {}
    i = 3
    while i < len(argv):
        if argv[i] != "--material":
            raise SystemExit(f"f3d_inject: unexpected argument {argv[i]!r}")
        name, kwargs = parse_material_arg(argv[i + 1])
        specs[name] = kwargs
        i += 2

    gltf = json.loads(in_path.read_text())
    materials = gltf.get("materials", [])
    if not materials:
        # Not a warning: the importer drops any primitive whose material is
        # missing or unnamed (parser.cpp:150-195), so this glTF would convert
        # to an empty model and the failure would surface much later as a
        # blank screen.
        raise SystemExit(f"f3d_inject: {in_path} has no materials; "
                         f"gltf_to_t3d would skip every mesh in it")

    default = specs.get("*")
    refs = {}
    for name, spec in specs.items():
        if spec.get("useRef"):
            refs.setdefault(str(spec.get("refAddress", "0x01")), []).append(name)
    for ref, names in refs.items():
        if len(names) > 1:
            # Tiny3D hashes a reference material by its number, so a second
            # material with the same number is not re-uploaded: it shows
            # whatever the first one uploaded. Sometimes that is the point.
            print(f"f3d_inject: warning: materials {names} share texture reference {ref}; "
                  f"they will show one image", file=sys.stderr)
    touched, skipped = [], []
    for mat in materials:
        name = mat.get("name")
        if not name:
            raise SystemExit("f3d_inject: a material has no name; gltf_to_t3d "
                             "keys its material table by name and drops the "
                             "primitive when it is empty")
        spec = specs.get(name, default)
        if spec is None:
            skipped.append(name)
            continue
        extras = mat.setdefault("extras", {})
        extras["f3d_mat"] = build_f3d_mat(**spec)
        touched.append(f"{name}={spec['preset']}")

    unknown = set(specs) - {"*"} - {m.get("name") for m in materials}
    if unknown:
        raise SystemExit(f"f3d_inject: --material named {sorted(unknown)}, "
                         f"but {in_path.name} has "
                         f"{sorted(m.get('name') for m in materials)}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    # sort_keys so two runs of the same inputs are byte-identical; the assets
    # check builds everything twice and compares.
    out_path.write_text(json.dumps(gltf, indent=1, sort_keys=True))

    print(f"  [F3D] {out_path.name}: {', '.join(touched) or 'nothing'}"
          + (f" (left alone: {', '.join(skipped)})" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
