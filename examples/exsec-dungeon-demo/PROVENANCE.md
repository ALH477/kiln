# `dungeon_*.gen.c` — provenance

Generated C. **Never hand-edit either file**; regenerate them. They are a dungeon
chunk generator — [`examples/dungeon/dungeon.exsc`](https://github.com/ALH477/exsecutor)
in the Exsecutor repository, a pure library — emitted by the Exsecutor compiler's
C backend, one unit per pointer width:

| | x86_64 unit | mips64 unit |
|---|---|---|
| source | `examples/dungeon/dungeon.exsc` (sha256 `8238bc26…b890`) | same |
| compiler | `exsc` at Exsecutor commit `6aae44d3137bad26bdd221df901ab18e922830ce` (512,240 bytes, sha256 `c238d44a70c927e4d4a081bfe04e9484a7c039fc9e4c5c68617f847aa6828613`) | same |
| row | `--hospes x86_64-linux --emitte c` | `--hospes mips64-none-o64 --emitte c` |
| bytes | 84,223 | 85,259 |
| sha256 | `4e36dc94d3fb81aab83ea134f42185282692fd1b526b8e9b7f8f296994cf7a86` | `c0628270bb86cdce0f8b889a8f33c2aab03793f8c2991515c048a993852c0298` |

The ROM links the mips64 unit. The x86_64 unit exists only for
`nix/checks/exsec-dungeon-parity.nix`, which compiles it natively.

To regenerate, from an Exsecutor checkout at that commit:

```
nix develop --command make all           # or: fasmg compiler/x86_64/exsc.asm build/exsc
build/exsc aedifica --hospes x86_64-linux --emitte c \
    examples/dungeon/dungeon.exsc -o dungeon_x86_64.gen.c
build/exsc aedifica --hospes mips64-none-o64 --emitte c \
    examples/dungeon/dungeon.exsc -o dungeon_mips64.gen.c
sha256sum dungeon_*.gen.c
```

The compiler binary above was assembled with nixpkgs' `fasmg` (`g.l8vn`) outside
the Exsecutor flake's devShell, which was not enterable in the session that
produced these files; whether the flake's pin yields the same 512,240 bytes is
**[UNTESTED]**. The emitted text is deterministic regardless (Exsecutor's
`make reproduce` checks this class of unit across working directory, time zone,
locale, `SOURCE_DATE_EPOCH`, umask and hostname); re-emitting the mips64 unit from
another directory here was byte-identical.

## The two units differ because `mensura` does

The mips64 row has a 32-bit `mensura` (a 64-bit ISA under an ABI with 32-bit
addresses). After renaming SSA values, every remaining difference between the two
files is one of: the `_Static_assert(sizeof(void *) == 8` vs `== 4`; an integer
width argument, `64` becoming `32`, on a checked add, subtract or multiply; an
inserted `exsi_norm_u(…, 32)`; or an array slot halving (`unsigned char s29[96]`
becoming `[48]`, since the ROM's 12-element `mensura` arrays are 4-byte). 562 lines
differ before the renaming, 448 after, and none of them is a behavioural edit. That
is `mensura` narrowing, said many times. `nix/checks/exsec-dungeon-parity.nix`
holds what must stay equal — the 17 exported prototypes — and nothing more.

## What it is certified against

In the Exsecutor repository the same source, driven by `probatio.exsc`, writes a
17,664-byte stream that is **byte-identical to `tests/programs/dungeon/expected.out`**,
which `prototypes/dungeon_oracle.py` (an independent Python implementation) wrote.
That holds on the reference backend, four C builds (gcc and clang, `-O0` and `-O2`,
under UBSan or its trap mode) and a big-endian MIPS-III run under qemu-user — the
**n32** ABI, as a proxy for this ROM's **o64** (spec §9.5; the o64 ABI itself is
`[UNTESTED]`). Seventeen mutants of the source are each caught at a stated byte
(that directory's `TEST` header). The emitted C also ran over 2,000,000 chunks
(`tests/c/dungeon_scan.c`) with no violation of tile codes, a solid border, or a
single 4-connected component, and no duplicate layout.

In THIS repository `dungeon_parity.c` re-runs that scenario on the committed
x86_64 unit against `dungeon_expected.bin` (a copy of that `expected.out`), and
checks `dungeon_golden.h` against the same stream. **`mips64-elf-gcc` 14.4.0,
`-Os -mabi=o64 -march=vr4300`, the toolchain this ROM is built with, has not been
run on any of it**; the byte-identity above is for a different compiler and ABI.

`dungeon_golden.h`'s four rows are records 9, 10, 14 and 16 of that stream
(world `0xDEADBEEFCAFEF00D`, indices 0, 1, 64 and 244,140,624,999), cross-checked
against the oracle's own generator when they were extracted.

## Measured with a different toolchain than this repo's

`clang --target=mips64 -mabi=n32 -march=mips3 -O2`, the unit alone:

- compiles clean at `-Werror -Wall -Wextra -fno-fast-math`;
- imports exactly `exsrt_abortus` (supplied by `main.c`). At `-Os` a compiler may
  add `memcpy`/`memset`, as it does for the StreamDB reader, and newlib resolves
  them; this was not observed here either way;
- contains **no floating-point instruction** — it is integer arithmetic throughout,
  so `-fno-fast-math` costs it nothing and the unit's `#error` on `-ffast-math` is
  its blanket refusal, not a constraint this code feels;
- one chunk executes **2,483,730 instructions** under `qemu-mipsn32
  -one-insn-per-tb -d exec` (seed `0x123456789`), 61% in the 3×3 neighbour count.
  At one instruction per cycle on a 93.75 MHz VR4300 that is a floor of 26.5 ms,
  for that compiler only. **Nothing has run on a VR4300 or in Ares**; the ROM
  displays its own measured time and stack depth, which would be the first.

Stack: no local array exceeds twelve words, and the buffers are the caller's, so
the frame should be small, but `-fstack-usage` has not been run with the ROM's
compiler. `main.c` runs the generator on a 32,768-byte thread, paints it, and
displays the measured peak.

## Licensing

Exsecutor is GPL-3.0-or-later with a stated exception: **code produced by the
compiler is not covered by the GPL** (Exsecutor's `LICENSE.EXCEPTION`, Exception
A). These generated files therefore carry no GPL obligation and sit under this
repository's MIT licence with the rest of `examples/`.
