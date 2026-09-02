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

## VADPCM codec (MPL-2.0) — host builds only

`plat/host/src/host_wav64.c` decodes `.wav64` sample data with **Dietrich
Epp's VADPCM codec**, which libdragon vendors at
`tools/audioconv64/vadpcm/` under the **Mozilla Public License 2.0**.
`nix/host.nix` compiles `codec/decode.c` and `codec/error.c` from the pinned
libdragon flake input; **no MPL source is vendored into this tree**, and
nothing on the N64 path links it — on console the decode is RSP microcode.

The reason it is that codec and not a decoder written here is that
`audioconv64`, which produced every `.wav64` in this repository, is built from
the same source: the host decodes with the decoder that belongs to the
encoder. `nix/host-math.nix`'s header makes the general form of this argument.

MPL-2.0 is file-level copyleft. It attaches to those two files and their
modifications (there are none — they are compiled as they stand), not to
`libkiln` or to anything else linked alongside. A binary distribution of a
host or WebAssembly build must say the codec is in it and where the source
is: <https://github.com/depp/vadpcm>, or the copy inside the libdragon
revision pinned in `flake.lock`.

One piece of the format is NOT this codec and is a transcription: the Huffman
layer that `audioconv64` wraps VADPCM in by default is decompressed on console
by the CPU, in a `static` function inside a libdragon translation unit that
cannot compile natively. `host_wav64.c` says so at the point of use.
