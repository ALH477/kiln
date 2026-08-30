# AGENTS.md — Kiln

You are in the **engine** repo (libkiln + Nix + `./dev`). Games are siblings, not submodules: `~/Documents/PetaByte-Madness`, `~/Documents/ganja-goblin`.

## Do this

1. `./dev help` — the CLI. Build/run/shot/drive/check/cheap/doctor/forge/poser live here.
2. Prefer a **jump ROM** over `./dev drive` for one screen or mode. Drive's uinput→SDL→ares chain is fragile; `./dev shot` has no input path. See `.claude/skills/n64-verify/SKILL.md`.
3. Visual/geometry/animation work: **read** `.claude/skills/n64-verify/SKILL.md`, `n64-modeling/SKILL.md`, `n64-animation/SKILL.md`, `n64-forge/SKILL.md` with the file tool. They are not Hermes-installed skills.
4. Fast gates (no ROM): `./dev cheap`
5. Full pre-push: `./dev check` — **not on the laptop while the user is playing unless they ask.**

## Do not

- `nix flake check` or a full ROM build unless asked.
- Push to `origin` (`github.com/ALH477/kiln.git`) unless asked.
- Reintroduce the name `M64` as an engine identifier (`nix/checks/kiln-names.nix`).
- Trust emulator audio. Validate audio on hardware.
- Type a dimension into C that a generator already emits. See n64-modeling.

## Layout

- `engine/src/kiln/` — one `MODULES` list in `engine/modules.mk`
- `nix/` — toolchain, libdragon, rom, faust gates, host math
- `plat/host/` — software rasteriser for sandbox-safe 2D/3D checks
- `./dev` — this tree only (`cd`s here). A game has its own `./dev`.
