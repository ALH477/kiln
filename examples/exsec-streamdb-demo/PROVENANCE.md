# `lector_streamdb.gen.c` — provenance

Generated C. **Never hand-edit it**; regenerate it. It is the StreamDB v3
reader written in [Exsecutor](https://github.com/ALH477/exsecutor), emitted for
this console by the Exsecutor compiler's C backend.

| | |
|---|---|
| source | `examples/streamdb/lector_streamdb.exsc` |
| compiler | `exsc` at Exsecutor commit `5950528` (455,662 bytes) |
| row | `--hospes mips64-none-o64` — 32-bit addresses, so a 32-bit `mensura` |
| bytes | 62,781 |
| sha256 | `f7e86542039fdddefd1721fd34d17d7364e8e5bd823cff5c492c47c8167de7a2` |

To regenerate, from an Exsecutor checkout at that commit:

```
nix develop --command make all
nix develop --command ./build/exsc aedifica --hospes mips64-none-o64 \
    examples/streamdb/lector_streamdb.exsc --emitte c -o lector_streamdb.gen.c
sha256sum lector_streamdb.gen.c
```

The emitted text is deterministic: Exsecutor's `make reproduce` checks that
this unit is byte-identical across working directory, time zone, locale,
`SOURCE_DATE_EPOCH`, umask and hostname.

## What it is certified against

In the Exsecutor repository, the same reader driven by `probatio.exsc` produces
a certificate stream byte-identical to the upstream C reader's verdicts over a
24-document container and three corrupted variants, under the compiler's
reference backend, four C builds (gcc and clang, `-O0` and `-O2`, all under
UndefinedBehaviorSanitizer), and big-endian MIPS emulation of this exact
`mips64-none-o64` row (Exsecutor spec §14 entry 25). This ROM adds a fourth
check that runs on the console itself: `main.c` looks every key up with Kiln's
own `streamdb-embedded` reader too, and shows whether the two agree.

## Measured with this repo's toolchain

`mips64-elf-gcc` 14.4.0, `-mabi=o64 -march=vr4300 -Os`, `-fstack-usage`:

| frame | bytes |
|---|---|
| `exs_arbor_percurre` | 20,680 |
| `exs_suffixum_percurre` | 8,584 |
| deepest callee (`exs_caput_elige`) | 328 |

`exs_arbor_percurre` measured 127,184 bytes before Exsecutor's ADR 0016, which
made `Arbor` a caller-supplied output through a mutable borrow. That is why
`main.c` can run the reader on a 32,768-byte kthread.

Under this repo's ROM flags plus `-fno-fast-math` and `-Werror`, the reader
object imports exactly `exsrt_abortus` (supplied by `main.c`) and `memset`
(produced by `-ftrivial-auto-var-init=pattern`), and contains no floating-point
instruction.

## Licensing

Exsecutor is GPL-3.0-or-later with a stated exception: **code produced by the
compiler is not covered by the GPL** (Exsecutor's `LICENSE.EXCEPTION`,
Exception A). This generated file therefore carries no GPL obligation, and sits
under this repository's MIT licence with the rest of `examples/`.
