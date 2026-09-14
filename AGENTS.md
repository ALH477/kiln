# AGENTS.md — Kiln

You are in the **engine** repo (libkiln + Nix + `./dev`). Games are siblings, not submodules: `~/Documents/PetaByte-Madness`, `~/Documents/ganja-goblin`.

## Do this

1. `./dev help` — the CLI. Build/run/shot/drive/check/cheap/doctor/forge/poser live here.
2. Prefer a **jump ROM** over `./dev drive` for one screen or mode. Drive's uinput→SDL→ares chain is fragile; `./dev shot` has no input path. See `.claude/skills/n64-verify/SKILL.md`.
3. Visual/geometry/animation work: **read** `.claude/skills/n64-verify/SKILL.md`, `n64-modeling/SKILL.md`, `n64-animation/SKILL.md`, `n64-forge/SKILL.md` with the file tool. They are not Hermes-installed skills.
4. Fast gates (no ROM): `./dev cheap` — ~7 s, and it now covers the `.map` path.
5. **Level work has a headless loop; use it rather than writing `.map` text.**
   `./dev map-emit spec.json out.map` authors from JSON, `./dev map-dump` reads a
   level back as JSON, `./dev map-validate --json` checks it, and
   `./dev map-render <file.map> out.png` DRAWS it with the real engine — no ROM,
   no emulator, no compositor. Hand-written `.map` text gets the brush winding
   wrong, which loads fine on console and yields zero geometry through the CSG;
   that is how six of this repo's seven `.map` files were broken.
   `./dev map-canon` repairs one.
6. To just *play* it: `./dev pc` (native window) or `./dev web --serve`
   (browser). Both run the real engine — no emulator, no ROM.
7. Kiln Studio (`./dev studio`, `:8420`) is the unified UI: builds, checks,
   the editors saving in place, a live game view. To drive it the way a person
   would — from an agent — `.mcp.json`'s **kiln-browser** server is a
   headless-Chromium MCP front-end confined to the studio's origin.
8. Full pre-push: `./dev check` — **not on the laptop while the user is playing unless they ask.**

## Do not

- `nix flake check` or a full ROM build unless asked.
- Push to `origin` (`github.com/ALH477/kiln.git`) unless asked.
- Reintroduce the name `M64` as an engine identifier (`nix/checks/kiln-names.nix`).
- Hand-edit a `*.gen.*` file, or restate the level vocabulary. Classnames,
  epairs, the engine's limits and the brush winding all come from
  `tools/schema/level_vocab.json`; `nix/checks/level-vocab.nix` fails on drift.
- Trust emulator audio. Validate audio on hardware.
- Type a dimension into C that a generator already emits. See n64-modeling.
- Give the host launcher its own renderer, or bless a per-architecture
  reference image. Both defeat the point — see CLAUDE.md, "One renderer,
  four architectures".

## Layout

- `engine/src/kiln/` — one `MODULES` list in `engine/modules.mk`
- `nix/` — toolchain, libdragon, rom, faust gates, host math
- `plat/host/` — software rasteriser for sandbox-safe 2D/3D checks, and the
  playable target: x86_64, wasm32, aarch64, riscv64, one set of reference
  images for all four (`nix/host.nix`)
- `plat/shell/` — the launcher: window, pad, sound. It blits; it must never
  rasterise
- `./dev` — this tree only (`cd`s here). A game has its own `./dev`.
