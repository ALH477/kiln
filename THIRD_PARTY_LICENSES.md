# Third-party and differently-licensed components

The Kiln engine itself (this repository, minus the exception below) is
MIT-licensed — see `LICENSE`. This file lists everything that isn't, and
why each one doesn't change that.

## streamdb-embedded/ — LGPL-2.1-or-later

DeMoD's own StreamDB reader (a bare-metal reimplementation of the upstream
v3 format sized for 4 MB RDRAM; upstream is `github:ALH477/DeMoD-StreamDB`).
Every file under `streamdb-embedded/` carries its own
`SPDX-License-Identifier: LGPL-2.1-or-later` header — those are unchanged by
this repository's relicense and should stay that way.

The engine links against it (`kiln_asset.c` includes
`<streamdb/streamdb_embedded.h>`) without relicensing it, which is exactly
what LGPL permits: a differently-licensed work may link against an LGPL
library as long as the library's own source stays available and unmodified,
which it does here — the full source is vendored in this repository,
unmodified, under its own license headers.

One honest caveat: LGPL's terms (particularly around a user's ability to
relink against a modified copy of the library) were written with dynamic
linking on a general-purpose OS in mind. This is a bare-metal N64 ROM with no
runtime linking at all, closer to firmware than to the case LGPL's text
anticipates. Shipping the complete, unmodified source — which this
repository already does — is the standard mitigation the embedded/firmware
community uses for exactly this situation, but it is not a substitute for
real legal advice if this distinction ever matters commercially for your use
of this engine.

## libdragon — Unlicense

The N64 SDK this engine builds on (`github:DragonMinded/libdragon`, `preview`
branch). Pulled in purely as a Nix flake input (`flake.nix`'s `libdragon`
input) and built by `nix/libdragon.nix` — **not vendored** as source in this
git repository. Its own license (Unlicense — public domain equivalent) is
unaffected by anything here.

## SummerCart64 — GPLv3

Provides `sc64deployer` (ROM upload, debugf stdio, IS-Viewer64) — a separate
tool, not a library linked into any ROM. Pulled in purely as a Nix flake
input (`flake.nix`'s `summercart64` input) and packaged by
`nix/tools/sc64deployer.nix` — **not vendored** as source. Its GPLv3 terms
apply to that tool alone and have no bearing on the engine or the ROMs it
builds, which never link against it.

## Faust DSP sources — reference only, not vendored

Any `.dsp` source referenced from a sibling `DeMoD` checkout
(`~/Documents/DeMoD/apps/terminus/patches/`, PolyForm Shield 1.0.0,
non-commercial) is referenced by path in documentation, never copied into
this repository. The `.dsp` files actually tracked here (`dsp/ks.dsp`,
`dsp/kiln_jingle.dsp`) carry this repository's own MIT license and are not
PolyForm-licensed content.

## Tiny3D, N64-UNFLoader, and other flake inputs

Every other dependency declared in `flake.nix`'s `inputs` (`tiny3d`,
`unfloader-src`, `claude-code-nix`, `nixos-generators`, `nixpkgs`,
`flake-utils`) is a pure Nix flake input, built at `nix build` time from its
own upstream repository, and never vendored into this git tree. Their
licenses are unaffected by and irrelevant to this repository's own license.
