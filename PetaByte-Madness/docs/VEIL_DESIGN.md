# THE SCARLET VEIL
### A red filter that costs nothing, and the four things that live inside it

---

## 1. The one idea

**The veil is not a filter. It is a palette swap.**

Author both states offline. At runtime, change only which lookup table you point at.

| layer | what changes | runtime cost |
|---|---|---|
| pixels | which TLUT is bound | 32 bytes of DMA per material |
| depth | fog colour + near/far registers | 2 register writes, already in pipeline |
| audio | which stem gain is up | mixer gain, zero DSP |

Everything below follows from that sentence.

---

## 2. Why not the obvious version

The default instinct is a full-screen alpha-blended red rectangle. On a 320×240 framebuffer that is **76,800 pixels of read-modify-write against the colour buffer, every frame.** Fill rate and RDRAM bandwidth are the N64's actual ceiling — not triangle count — so that one quad is the most expensive thing on screen, and it competes directly with the demons you want to draw *because of* the filter.

The palette swap gets you a better-looking result for free, and it does it by using the hardware the way it was designed:

| approach | extra pixels shaded/frame | TMEM | per-frame CPU |
|---|---|---|---|
| full-screen blend quad | **76,800** RMW | 0 | 0 |
| **VEIL TLUT swap** | **0** | +32 B per material | one pointer index |
| fog crimson shift | 0 (already in blender) | 0 | 2 registers |
| vignette ring *(optional)* | ~20,800 | 0 | 8 triangles |

Measure these on hardware before you trust any of it — but the *ordering* will hold on any RDP.

### And it pays for itself

Under the veil the horizon is opaque crimson anyway. So when the veil rises, **pull the far plane in.** Fog hides the cut completely. You get the frame time for the extra demons out of the filter that summoned them. `veil_t` carries `far_normal` and `far_veiled` for exactly this.

---

## 3. The four layers

1. **TLUT swap** *(the whole effect)* — every material is CI4. Two 16-entry palettes per texture, cold and veiled, with 9 pre-baked blend steps between them. 288 bytes of ROM per material buys a fully animated crossfade for one pointer add. `veil_bind_palette()`.
2. **Fog** — colour lerps toward deep arterial, near-plane pulls in hard. Fog is computed in the RSP vertex pipeline and applied in the blender; if your scene already fogs, this is genuinely free. `veil_apply_scene()`.
3. **Step quantisation** — the crossfade is deliberately coarse: 9 steps over 400 ms is ~5 frames of visible stepping at 30 fps. That reads as a hard *optical* snap, like a filter wheel dropping into place, rather than a soft videogame fade. Keep it coarse on purpose.
4. **Vignette ring** *(cut this first if you're over budget)* — 4 trapezoids, 8 triangles, outer band only. Not a rectangle. About a quarter the fill cost of a full-screen pass.

---

## 4. The palette contract

This is the art rule that makes the whole thing legible, and it's non-negotiable once you start authoring textures.

> **Under the veil, hue carries no information. Only value does.**

So value gets rationed:

- **World / environment TLUTs are clamped to the middle value band.** They collapse to a narrow crimson mid-range. Readable, but flat.
- **Demon TLUTs get the full range.** Demons are the only things on screen permitted to touch true black and true white.

The result: with the veil up, your eye is *physiologically* dragged to the demons. Not by an outline shader, not by a highlight — by the fact that they are the only source of contrast in the room. See `palette_contract.png`.

### The phantom rule (this replaces the first draft's "low contrast" cold state)

A demon's veil-off palette is not dim. **Every one of its 16 entries has alpha 0.**

In RGBA5551 the alpha bit *is* the transparency bit, so under `G_RM_AA_ZB_TEX_EDGE` every texel of the creature fails alpha compare and no pixel is written. Which means you shouldn't walk the display list to find that out: `veil_demon_submit()` returns false at step 0 and the body is never submitted. **The creature costs exactly zero while you can't see it.** A corridor can hold a dozen of them for free.

The one exception is the eyes — a separate material whose cold palette keeps alpha on its top four entries. Four triangles per demon. Veil down, you get pinpricks in the dark and nothing else. That exception is the whole reason the trick is frightening rather than merely cheap.

You can't crossfade a 1-bit alpha, so the 400 ms transition runs bodies on an XLU pass with `gDPSetPrimColor` alpha = step, then switches to the opaque TEX_EDGE pass at full veil. Two render modes, one branch, a few hundred blended pixels for four tenths of a second. See `renders/shot_phantom.png`.

---

## 5. The demons

Classic anatomy — horns, hooves, bat wings, barbed tails, fangs — because under the veil silhouette and value are the only channels you have, and a horned outline is unmistakable at thirty pixels tall.

| | tris | verts | animations | veil property |
|---|---|---|---|---|
| **IMP** | 440 | 461 | 5 | the phantom baseline |
| **HELLHOUND** | 466 | 528 | 5 | only *moves* while the veil is down |
| **GARGOYLE** | 552 | 595 | 5 | statues stand where it does |
| **OVERLORD** | 600 | 623 | 5 | forces the veil on in its radius |

2,058 triangles for the set, each under 1,088 B of TMEM. All rigs are **rigid segmented hierarchies** — no vertex skinning — so each part is one node with TRS keys, mapping 1:1 onto the N64 matrix stack. Budget honestly: three imps and a hellhound is a reasonable encounter; four overlords is not.

---

### IMP — *the common enemy*
Pot-bellied, long-armed, goat-legged. Two curled horns, big pointed ears, a whip tail with an arrowhead barb. Scampers on a real walk cycle, rears up to swipe.

`idle_crouch` · `scamper` · `leap` · `swipe` · `death`

The teaching enemy. First time you turn the veil on in a room you thought was empty, there are three of them and one is close.

---

### HELLHOUND — *the stalker*
Quadruped, bony, horned skull, dorsal spine ridge, long tail.

**It only moves when the veil is down.** Veil up and it freezes exactly where it is — visible, motionless, nearer than it was. Its `frozen` clip is deliberately almost nothing: a shallow breath and a slow pulse in the eyes. The stillness is the horror; the player's instinct is to hold the veil up forever, and §6 is what stops them.

`frozen` · `prowl` · `charge` · `bite` · `death`

---

### GARGOYLE — *the ambush*
Winged, crouched, stone-skinned, with something burning inside the shell.

Because a phantom isn't drawn at all with the veil down, you can put a **real statue prop in the same spot**. Veil down: a statue, identical to the forty other statues bolted around the level. Veil up: the statue is gone and a gargoyle is standing there. The shell material erases to reveal the ember core, and it unfurls.

The level teaches you to stop trusting statuary, which is free level design falling out of a rendering property.

`perch_statue` · `unfurl` · `hover` · `dive` · `death`

---

### OVERLORD — *the elite*
Two and a half metres. Ram horns sweeping back, a burning crown, exposed ribs, hooves, four-fingered claws.

Inside its halo radius `veil_force()` overrides the player's toggle and pins the veil **on**. It can't blind you — only expose you, and take away the tool you manage every other demon with. Fight one alongside a hellhound and the design closes: the overlord holds the veil up, so the hellhound is frozen — until you kill the overlord, the crown collapses, the veil drops, and everything you froze starts moving at once.

`idle_smolder` · `stomp` · `roar` · `cleave` · `death`

---

## 6. The economy: why you ever turn it off

The veil is free on the hardware. It must not be free in the fiction.

**The rule: the veil is reciprocal. While it's up, they can see you too.**

Everything falls out of that one line:
- The HELLHOUND freezes under the veil because it's being observed — and observation goes both ways.
- The GARGOYLE only ambushes what has looked at it.
- The IMP swarm knows exactly where you are the moment you resolve them.
- The OVERLORD is terrifying because it removes your ability to stop being seen.

**Where it comes from:** Horner's MRI. He climbs onto that machine and it takes him in — and what he comes out with is the veil. It's not a gadget he found; it's a change to him that he cannot switch off permanently, which is exactly why the mechanic has a cost. The most depressed man in the underwater lab gave himself the ability to see what's actually down there.

---

## 7. Audio — the same architecture

`veil.dsp` implements the sentence from §4 with one word changed:

> Under the veil the world loses its **high frequencies**. Demons keep them.

Localisation cues live almost entirely in HF content. So the veil doesn't just make demons *look* like the only contrast in the room — it makes them the only sound you can place in space.

Split it the way the pixels split:
- **World → bake offline.** Render every ambience stem twice through `veil.dsp` (veil=0, veil=1), ship both, crossfade mixer gains. One extra voice per stem, zero DSP.
- **Demon voices → run live.** One biquad and a shelf on a handful of voices, only while demons are audible.

`pulse` (F0/16 ≈ 6.5 Hz, phase-locked to the drone) is the gameplay clock — read it on the CPU for ANTIPHON's window and the CARDINAL's halo.

---

## 8. Integration checklist

1. Convert every material in the demon zone to **CI4**. This is the real work; everything else is an afternoon.
2. Author two palettes per texture. Environment → mid band. Demons → full range. Bake the 9 blend steps (`veil_tluts.h` shows the format).
3. `veil_update()` once per frame → `veil_apply_scene()` before the world → `veil_bind_palette()` per material.
4. Wire the far-plane rebate. Don't skip this; it's where the demons' frame time comes from.
5. Flag every demon material `VEIL_PAL_PHANTOM`, and its eye material `VEIL_PAL_EYES`. Gate the body display list on `veil_demon_submit()`.
6. Feed `veil_audio_t()` to the mixer crossfade.
7. Vignette last, and only if you have the fill rate.

**Check the `rdpq_*` signatures against your libdragon revision** — that API has moved. The four hardware touchpoints are marked `VEIL_HW_*` in `veil.c`, with libultra `gDP*` equivalents in comments beside each.

---

## 9. What the render pass proved (and three things it broke)

Everything in `renders/` comes out of a software rasteriser that writes an
**index buffer**, not colour — per pixel it stores material id, CI4 texel index,
shade and depth, which is what the RDP has in flight before the TLUT lands.
Colour only exists at compose time, so `shot_money.png` is one rasterisation
pass composed twice with two palettes. It is the mechanism, not a repaint.

Measured off the corridor frame, red channel, with the veil up: demon p95 lands
at **2.2× the brightest wall** while their p05 stays below the wall median.
They own both ends of the range. With the veil down there is nothing to measure
— the bodies aren't submitted.

### Two authoring findings

**1. Demons must draw near-DECAL, not modulated.** Run enemies through the same
modulate combiner as the room and they come out *darker* than the walls — the
exact inverse of the design. Vertex colour × lighting × TLUT is three
multiplications and the bright end of the palette never survives it. Use
`G_CC_DECALRGBA` (or a shade term close to 1.0) under the veil. It's a combiner
mode, not a pass, so it's free.

**2. Enemy vertex colours need a floor.** If a vertex colour crushes toward
zero, no palette entry can rescue it. The generator lifts every enemy vertex
colour to `0.20 + 0.72·c`. Enemies get their own `gSPSetLights` on N64 anyway;
this is the same idea moved into the asset.

### Three bugs the renders caught

These are worth naming because they're all silent — the geometry builds, the
glTF validates, and the model is simply wrong.

1. **`(x,y,z) → (x,z,y)` is a reflection, not a rotation.** It flips handedness
   and inverts every normal. The hellhound's skull was pointing backwards out
   of its own neck. The rotations you want are `(x, z, -y)` for forward and
   `(x, -z, y)` for back.
2. **Rotating an arm about Z by `-k·sx` swings it *into* the torso.** For a left
   arm at negative X you need a negative angle, so the sign is `+k·sx`. Both
   arms were buried inside the silhouette on all three bipeds.
3. **Tail chain base rotations were positive-X**, which lifted the tails and
   exactly cancelled the droop joint they hang from. They read as broom handles
   until the sign flipped.

### On animation

`anim.py` resamples a small number of authored poses into denser eased keys, so
glTF's LINEAR interpolation traces an arc instead of a straight line. That plus
three things: **overlap** (tails, wings and ears lag their parent down a chain),
**counter-rotation** (hips and shoulders turn against each other on locomotion),
and **anticipation** (a real crouch before a leap, a wind-back before a swipe).

Roughly 800–1,200 keyframes per creature, up from ~150 in the first pass.
Keyframe data is cheap; robotic motion is not.

---

## 10. Renders

| file | what |
|---|---|
| `shot_money.png` | an empty corridor with six red pinpricks, then three demons |
| `shot_phantom.png` | 0 triangles → XLU crossfade → 440 triangles |
| `phantom_reveal.mp4` | the same thing in motion, on one imp |
| `shot_veil_compare.png` | off / crossfade / on, 320×240 native |
| `veil_toggle.mp4` | the filter rising and falling in the corridor |
| `overlord_roar.mp4` | the elite forcing the veil up against your will |
| `turntable_<demon>.png` | 6 angles, budget in the header |
| `poses_<demon>.png` | all five animations, 6 poses each |
| `<demon>_anims.mp4` | every animation played through |

Every clip ships as `.mp4` and `.gif`. Everything is rasterised at 320×240 and
point-upscaled, so the pixel count you see is the pixel count you get.

---

## 11. Files

| file | what |
|---|---|
| `imp.glb` `hellhound.glb` `gargoyle.glb` `overlord.glb` | rigged, 5 animations each, rigid hierarchies |
| `veil.h` `veil.c` | the filter runtime |
| `veil.dsp` | Faust, world + demon buses, gameplay pulse |
| `veil_tluts.h` | generated cold/veil palettes, RGBA5551 |
| `*.ci4` | packed CI4 index maps, N64 byte order |
| `*_veil_on.png` `*_veil_off.png` | both states of each texture |
| `*_tluts.png` | per-material palette strips |
| `palette_contract.png` | the value-band rule, one image |
| `bestiary.py` `parts.py` `anim.py` | the creatures, the anatomy primitives, the animation library |
| `build_demons.py` `gltfkit.py` `textures.py` | entry point, glTF/GLB writer, CI4 + TLUT generator |
| `softrend.py` `render_shots.py` `stagelib.py` `shots.py` | the index-buffer rasteriser and the shot list |
