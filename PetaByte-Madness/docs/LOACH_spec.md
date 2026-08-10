# DSV LOACH — one-person work submarine

*Horner Station tender. Built in the moon pool, not in a yard.*

---

## What it is

A single-seat wet-hatch work sub, assembled from salvage by one competent,
exhausted person. It is not a nice boat. Nothing on it was styled, nothing was
faired, nothing was painted twice. It is also not a bad boat: everything on it
was done in the right order by somebody who understood what would kill him.

- **Hull:** a 1,900 L LPG tank, ends cut off and re-dished, two welded
  stiffener rings added at the frame stations. The original ochre tank paint is
  still on it, gone brown, streaked from the anodes down.
- **Viewport:** 60 mm cast acrylic hemisphere on a bolted flange, seated on a
  face seal. Scratched to a haze at the outer edge; he polishes the middle.
- **Access:** one 500 mm hatch aft of the dome, four dogs, red-lead primer,
  never topcoated. Entry is bow-down from the lab hallway.
- **Propulsion:** rewound 24 V trolling motor in a fabricated shroud, three
  stator struts, four-blade bronze prop. Loud. Reliable.
- **Power/trim:** two ex-fire-extinguisher bottles in band clamps as battery
  pods. They don't match — different lengths, different diameters — because
  that's what was in the store. The clamps are correct and evenly spaced.
- **Lighting:** two automotive work lamps on a length of angle iron. Cable runs
  in conduit along the starboard shoulder into a junction box, strain-relieved
  at both ends.
- **Other:** rebar-and-netting specimen basket under the bow (he's a biologist).
  Two doubler plates welded over hull thin spots. Scaffold-tube landing skids on
  three pad-eye struts a side, sized to the lab deck plating.

### Specification

| | |
|---|---|
| Length overall | 2.86 m |
| Beam over pods | 1.44 m |
| Height over hatch | 1.34 m |
| Crew | 1, seated, knees up |
| Endurance | ~6 h at loiter, less if the lamps are on |
| Comms | acoustic link back to the station only — no surface leg |
| Comfort | none. No head, no heater, condensation runs down the inside of the dome |

The one honest failure: the port lamp circuit floods at depth and has been
"fixed" three times. He goes out with one lamp and doesn't mention it.

---

## N64 asset

| | |
|---|---|
| Triangles | 536 |
| Vertices | 434 (18 batches, ≤32 each — F3DEX2 vertex cache safe) |
| Texture | one 32×32 RGBA5551 tile, 2 KB (half of TMEM), `G_TX_WRAP` both axes |
| Shading | **baked into vertex colour** — vertical AO gradient, per-facet rust bias. Run with `G_LIGHTING` off |
| Units | s16 centimetres, +Z forward / +Y up (286 units long) |

### Draw

`loach_dl` handles the geometry-mode changes for you:

```c
gsSPClearGeometryMode(G_LIGHTING);
gsSPSetGeometryMode(G_CULL_BACK | G_SHADING_SMOOTH);
/* hull, ribs, dome, hatch, pods  — closed solids, cull on */
gsSPClearGeometryMode(G_CULL_BACK);
/* skids, shroud, prop, lights, basket, fins — single-sided plates, cull off */
```

Combiner: `G_CC_MODULATERGB` for `hull`/`ribs` (the texture is only mapped
there), `G_CC_SHADE` for everything else. If you'd rather not bind a texture at
all, run the whole thing on `G_CC_SHADE` — the vertex colours carry it, and you
lose 2 KB of TMEM pressure for free.

### Per-group display lists (all exported separately)

`hull ribs dome hatch shroud prop skids pods lights basket fins`

Useful because:

- **`loach_dl_prop`** — push a matrix, spin it on Z, tie the rotation rate to the
  thruster Faust voice so the visual RPM and the hum are the same number.
- **`loach_dl_hatch`** — hinge it as its own matrix for the entry animation off
  the lab hallway.
- **`loach_dl_lights`** — swap the lens vertex colour to kill the port lamp when
  it floods. Two byte writes, and the story lands.

### LOD

For a distant/parked draw, submit `hull + ribs + dome + hatch + skids` only:
300 tris, and the silhouette is unchanged from more than about 6 m out.

---

## Files

| File | Use |
|---|---|
| `loach_preview.png` | four-view flat-shaded reference |
| `loach.h` | F3DEX2 `Vtx` array + per-group display lists + combined `loach_dl` |
| `loach_hull_rgba16.h` | 32×32 RGBA5551 texture array for `gsDPLoadTextureBlock` |
| `loach.obj` / `.mtl` | Blender/toolchain import, vertex colours in the `v` lines |
| `loach_hull.png`, `loach_hull_x8.png` | texture, 1× and 8× nearest for inspection |
| `build_loach.py` | the generator — edit this, not the header |

## Tweaking

Everything is one parameterised script. The knobs worth turning:

- `NSIDE` (10) — hull sides. 8 drops ~30 tris and reads fine at N64 resolution;
  12 is the point where you're paying for silhouette nobody sees.
- palette constants at the top (`HULL`, `HULL_RUST`, `DOME`, …) — the rust bias
  is applied per hull facet from a seeded RNG, so `random.seed(1977)` gives you a
  different but equally plausible weathering pass.
- `hull_profile` — the (z, radius) lathe profile. Widen the mid section and the
  boat gets fatter without any other edit.
- `hull_patch(...)` calls — add doubler plates anywhere on the hull for 2 tris
  each. This is the cheapest "somebody has been repairing this for years" detail
  in the whole model.
