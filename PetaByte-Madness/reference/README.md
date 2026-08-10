<!-- SPDX-License-Identifier: MPL-2.0 -->
# reference/ — F3DEX2 dumps

These headers are **data, not build inputs.** Nothing here is compiled into
the ROM and nothing in `src/` includes them.

They are libultra-style exports: `Vtx` arrays plus `Gfx` display lists built
with `gsSP*`/`gsDP*` macros, the output format of the generators in
`tools/`. This engine does not consume that format — geometry goes through
`gltf_to_t3d` into Tiny3D `.t3dm`, and the display list is built on the RSP
by Tiny3D's own microcode. Handing `loach.h` to this build system is not a
one-line change; it is a different renderer.

What they are still good for:

- **Exact vertex data and topology** for models whose `.obj` is also in
  `assets/obj/` — useful when checking whether a Blender import round-tripped
  correctly.
- **The per-group display-list split**, which is real design information:
  `loach.h` exports `loach_dl_prop`, `loach_dl_hatch`, `loach_dl_lights` etc.
  separately so the propeller can spin on its own matrix and the flooded port
  lamp can be killed with two byte writes. `docs/LOACH_spec.md` explains why.
  Any Tiny3D port of these models should preserve that split as separate
  meshes.
- **Baked vertex colours**, including the "lift every enemy vertex colour to
  `0.20 + 0.72·c`" floor that `VEIL_DESIGN.md` §9 describes. That floor is a
  real finding — without it no palette entry can rescue a crushed vertex —
  and it lives in the data, not in any script that survived the drop.

| file | model |
|---|---|
| `loach.h`, `loach_hull_rgba16.h` | the DSV LOACH work submarine |
| `patrick_horner.h`, `ph_rig.h`, `ph_anim_intake.h` | Horner, his rig, and the MRI intake animation |
| `machine_centaur.h` | the cyborg centaur |
| `dank_lab.h` | the lab interior (149 KB — the largest thing in the drop) |
