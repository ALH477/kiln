# Naming convention — Kiln / Figulina / Exsecutor

Status: ADOPTED 2026-09-26 (migration in progress — see section 9)
Date: 2026-09-26
Applies to: `ALH477/kiln`, `ALH477/exsecutor`, and the Exsecutor runtime used for bit-for-bit alignment
Does not replace: Exsecutor ADR 0005 (bespoke Latin lexicon inside `.exsc`)

## 1. The sentence

Kiln fires Figulina.
Figulina runs on the Exsecutor runtime.
Signaculum is how you know the frame did not drift.

A name that cannot be placed in one of those four roles is the wrong name.

## 2. Canonical names

| Role | Public name | Spoken | Repo (now) | Repo (later, optional) |
|---|---|---|---|---|
| Forge — Nix flake, toolchain, Faust bake, ROM builder, gates | **Kiln** | kiln | `ALH477/kiln` | stays |
| Ware — game engine linked into a ROM | **Figulina** | fig-oo-LEE-nah | still inside `kiln/` as `engine/` | `ALH477/figulina` only after symbols move |
| Law — language, compiler `exsc`, capability runtime | **Exsecutor** | ex-SEK-yoo-tor | `ALH477/exsecutor` | stays |
| Seal — golden-vector / framebuffer certificate | **Signaculum** | sig-NAK-yoo-lum | a program + a verdict, never a library product | stays in Exsecutor; Kiln may host a demo |

Rejected as product names: Fornaciter, Fornax (keep as etymology only), Calcaria (etymology only), libfigulina (legacy).

## 3. What each name is allowed to mean

### Kiln

The chamber. Hermetic inputs, pinned toolchain, `nix flake check`, `lib.mkN64Rom`, `lib.mkFaustVoice`, `lib.mkBakedInstrument`, asset pipelines, Ares/SC64 apps.

Kiln may say “this ROM is well-formed.”
Kiln may not say “these pixels match the other bench.” That is Signaculum.
Kiln may not own actors, rooms, cameras, or the frame loop. That is Figulina.

### Figulina

The vessel that comes out of the kiln. Tiny3D scene, rdpq immediate-mode HUD, actor pool, room streaming, follow camera, skeletal animation, RSP mixer consumption, StreamDB residency.

Figulina may call the Exsecutor runtime.
Figulina may not define target, locale, clock, rounding, or byte order. Those are capabilities `poscit`’d from Exsecutor.
Figulina may not bake Faust or stamp a `.z64` header. That is Kiln.

### Exsecutor

The language and the runtime in which locale and target are capabilities, never ambient state. Compiler `exsc`. Types such as `acies`. Time such as *tempus* / *metronomus*. Wire codecs certified against golden vectors.

The “full Exsecutor runtime for bit-for-bit alignment” lives **here**, not under Figulina.
A Figulina frame that is bit-identical across x86-64, qemu-musl-mips64, and VR4300 is an Exsecutor program presenting through Figulina, sealed by Signaculum.

### Signaculum

A certificate, a demo, and a verb.

- As artifact: the logo raster and its `expected.out`.
- As protocol: CRC (or a stronger digest) of a defined buffer against a baked constant.
- As verdict: `AGREE` or `DISAGREE`. No third state in CI.

Signaculum is not an engine, not a renderer brand, and not a flake.

## 4. Prefix registry

One prefix per layer. Do not invent a second prefix because a file felt lonely.

| Layer | Prefix | Examples | Notes |
|---|---|---|---|
| Kiln CLI / Nix | `kiln` (flake), `./dev` | `nix build .#hello`, `./dev shot`, `./dev deploy` | Human-facing forge words stay English-short |
| Kiln Nix libs | `mk` | `lib.mkN64Rom`, `lib.mkFaustVoice` | Already the kiln API |
| Figulina C | `fig_` | `fig_frame_begin`, `fig_scene_begin`, `fig_gui_text` | Three letters. Not `figulina_` |
| Figulina library | `libfigulina` | `libfigulina.a` | Full word only on the archive and package |
| Exsecutor C / emitted | `exs_` | `exs_signaculum_pingue`, `exs_acies_*` | Anything `exsc` emitted or the runtime owns |
| Exsecutor compiler | `exsc` | binary, `.exsc` sources | Not `exs` for the compiler itself |
| Exsecutor diagnostics | `EXS-E` | `EXS-E0601` | Unchanged |
| Signaculum | none as API | log line `signaculum: AGREE` | Do not create `sig_` |
| Vendored / upstream | keep theirs | `t3d_`, `rdpq_`, `streamdb_`, Faust | Never re-prefix |

Collision rule: if a symbol needs two prefixes, the layer cut is wrong. Split the function.

## 5. Identifier laws

### 5.1 Figulina C (libdragon / Tiny3D side)

```
fig_<object>_<verb>
fig_<object>_<verb>_<detail>
```

- `object` is a Figulina noun: `frame`, `scene`, `gui`, `actor`, `room`, `camera`, `skel`, `mix`, `res` (residency).
- `verb` is English and short: `begin`, `end`, `submit`, `spawn`, `tick`, `draw`.
- No Latin roots in the C prefix layer. Latin is for product titles and for `.exsc`.
- Immediate-mode GUI keeps `fig_gui_*`. Do not grow a retained widget vocabulary without an ADR.

Existing names map 1:1:

| Legacy | New |
|---|---|
| `fig_frame_begin` | `fig_frame_begin` |
| `fig_scene_begin` | `fig_scene_begin` |
| `fig_gui_begin` | `fig_gui_begin` |
| `fig_gui_panel` | `fig_gui_panel` |
| `fig_gui_text` | `fig_gui_text` |
| `fig_gui_bar` | `fig_gui_bar` |
| `fig_gui_end` | `fig_gui_end` |
| `fig_frame_end` | `fig_frame_end` |
| `fig_soft3d` | `fig_soft3d` (host); calls into `exs_*` |

During migration, `kiln_*` may exist as macros pointing at `fig_*`. Macros die when the last in-tree call site is gone. No public header ships both as first-class.

### 5.2 Exsecutor identifiers

Unchanged: prefix-root-suffix from the versioned morpheme table; suffix carries the type contract; ADR 0005 is load-bearing.

Figulina must not invent Exsecutor names. If Figulina needs a new capability (clock, target, rounding mode), it is declared in Exsecutor and granted through `poscit`.

### 5.3 Files and packages

| Thing | Pattern |
|---|---|
| Figulina public headers | `engine/include/figulina/<noun>.h` |
| Figulina sources | `engine/src/figulina/<noun>.c` |
| Soft-3D bridge to Exsecutor | `engine/src/figulina/soft3d.c` wrapping `exs_*` |
| Provenance of emitted blobs | `engine/src/figulina/gen/PROVENANCE.md` |
| Nix engine derivation | `packages.figulina` (alias `packages.engine` until cut) |
| Worked ROM | `packages.figulina-demo` (alias `engine-demo` until cut) |
| ROM title string | `FIGULINA` / `FIGULINA DEMO` — 20-char N64 limit, ASCII |
| Exsecutor sources | `*.exsc` |
| Golden outputs | `*.expected` / `expected.out` |
| Signaculum demo | `examples/exsec-signaculum-demo/` until it moves with the engine |

N64 header titles stay uppercase ASCII. No macron, no æ.

### 5.4 Nix output names

- Forge outputs keep kiln-flavoured flake paths: `packages.toolchain`, `packages.libdragon`, `lib.mkN64Rom`.
- Engine outputs use figulina: `packages.figulina`, `packages.figulina-demo`.
- Dual names are allowed for one release train. The alias that is not canonical is marked in the flake description string, then deleted.

## 6. Runtime boundary

The Exsecutor runtime is the bit-for-bit surface. Figulina is a client.

Allowed in Figulina without `poscit`:

- RDP state the engine itself transitions (`fig_scene_begin` → `fig_gui_begin` is one documented seam).
- Actor IDs, room keys, camera spring constants — game vocabulary.

Forbidden in Figulina as ambient state:

- Host locale, encoding, endianness, pointer width.
- Clock source and tick epoch (*tempus* / *metronomus* belong to Exsecutor).
- Float rounding, contraction, and lane width (`acies<f32,8>` vs `acies<f64,8>`).
- “Whatever Ares did this time.” Emulators are witnesses, not oracles. Audio work is hardware or a certified dump.

A function that changes pixels and does not go through a certified primitive is either (a) explicitly non-certified (document it) or (b) a defect.

## 7. Signaculum protocol (name-level)

Log / ISViewer line:

```
signaculum: AGREE  crc=…  expect=…  stack=…/…
signaculum: DISAGREE crc=…  expect=…
```

- The first token is always `signaculum:`.
- The second token is only `AGREE` or `DISAGREE`.
- CI greps that line. Do not paraphrase.

What AGREE is allowed to claim must be written next to the constant it checks (see existing `PROVENANCE.md` pattern): benches, toolchain, buffer size, letterbox, digest.

## 8. Metaphor budget

These words are reserved. Using them for anything else is a layer violation.

| Word | Means |
|---|---|
| kiln | the forge |
| fire / bake | Kiln transforming source → ROM / VADPCM / `.t3dm` |
| figulina | the engine / the ware |
| clay | upstream inputs (libdragon, Tiny3D, StreamDB, Faust `.dsp`) |
| signaculum | the seal |
| exsecutor | the law |
| acies | packed lanes in Exsecutor |
| poscit | capability demand |
| metronomus / tempus | certified time |

Do not introduce Fornax, Athanor, Calcaria, or Fornaciter as code identifiers. They may appear once in a design note as etymology.

## 9. Migration

Do this in order. Do not rename the GitHub repo first.

1. Write this document into `kiln/docs/NAMING.md` (or `docs/decisions/0001-figulina.md`).
   DONE — this file.
2. Add `fig_*` as the real symbols; `kiln_*` becomes a compatibility macro in one header.
3. Rename `libfigulina.a` → `libfigulina.a`. Keep a `libfigulina.a` symlink for one train.
4. Point `packages.engine` at the same derivation as `packages.figulina`.
5. Change ROM titles and demo directory names.
6. Delete `kiln_*` macros when the tree and the Exsecutor-emitted C no longer mention them.
7. Only then consider `ALH477/figulina` as a split. The flake named kiln should still *build* figulina.

Stop condition for the rename: `git grep -n 'fig_frame_\|libfigulina\|Figulina'` is empty in engine sources, and the remaining “Kiln” hits are forge docs and the flake description.

## 10. Forbidden

- One word for two layers (“Figulina” as a product phrase).
- `fornaciter_` anything.
- Re-prefixing Tiny3D / rdpq / StreamDB.
- A Figulina clock.
- A Signaculum “framework.”
- English marketing names beside the Latin product names (*ClayEngine*, *N64Forge*) unless they are throwaway section headings.
- Shipping `figulina_frame_begin` after this document exists.

## 11. One-line glossary for READMEs

> **Kiln** is the hermetic Nix forge for N64 / Analogue 3D / ModRetro M64.
> **Figulina** is the engine it fires (`libfigulina`, `fig_*`).
> **Exsecutor** is the language and runtime that forbids ambient target.
> **Signaculum** is the seal: AGREE, or the ROM does not ship.
