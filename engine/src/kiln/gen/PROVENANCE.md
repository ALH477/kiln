# `signaculum_*.gen.c` — provenance

Generated C. **Never hand-edit either file**; regenerate them. They are the
Exsecutor logo's software rasterizer — `signaculum_pingue`, the pure core in
[`examples/signaculum/forma.exsc`](https://github.com/ALH477/exsecutor) —
emitted by the Exsecutor compiler's C backend, one variant per pointer width:

| | x86_64 variant | mips64 variant |
|---|---|---|
| source | `examples/signaculum/forma.exsc` | same |
| compiler | `exsc` at Exsecutor commit `a413e6f26f7954c19de69aabf6af8378d0358f44` (488,331 bytes) | same |
| row | `--hospes x86_64-linux` | `--hospes mips64-none-o64` — 32-bit addresses, 32-bit `mensura` |
| bytes | 49,958 | 50,230 |
| sha256 | `8776cbd3ba0c1346ab28cc441b15ae0932239bdf4da68e81d6dc1a4979112b37` | `f43f4eae210a307bbf55311a0074a5961b87fafb2a03bc0219ca99de1c5653fc` |

To regenerate, from an Exsecutor checkout at that commit:

```
nix develop --command make all
cp build/exsc /tmp/exsc_a413e6f          # pin the binary before use
/tmp/exsc_a413e6f aedifica --hospes x86_64-linux --emitte c \
    examples/signaculum/forma.exsc -o signaculum_x86_64.gen.c
/tmp/exsc_a413e6f aedifica --hospes mips64-none-o64 --emitte c \
    examples/signaculum/forma.exsc -o signaculum_mips64.gen.c
sha256sum signaculum_*.gen.c
```

Both variants compile from `kiln_soft3d.c` — the console picks the mips64
one (`#if defined(N64)`, the tier macro n64.mk sets and the host tier never
does), the host tier picks the x86_64 one. The mips64 unit carries
`_Static_assert(sizeof(void *) == 4)` and **cannot compile for the host**;
that is why there are two pinned files rather than one.

## The EXSG stream

`examples/exsec-signaculum-demo/filesystem/signaculum.exsg` is a byte-for-byte
copy of Exsecutor's `tests/data/signaculum_mesh.bin` at the same commit
`a413e6f` — 44,801 bytes, sha256
`07535d3c33f7d72a57db9e901a6cda83210a79d2c56a049e21854e88f958ecf1`, written by
that repo's `prototypes/signaculum_mesh.py` (the 1,493-vertex / 2,981-face
fixed-point description of `logo/Meshy_AI_Crossed_Ascension_0910024256`).

## What it is certified against

In the Exsecutor repository the same core, driven by the
`signaculum.exsc` stream pump over this blob, produces a P6 stream
**byte-identical to `tests/programs/signaculum/expected.out`** (196,623 bytes;
the 256×256 RGB888 pixel payload is its last 196,608 bytes) on x86-64 and on
big-endian mips64 under qemu-user (the reference backend plus gcc/clang C
builds at -O0/-O2; Exsecutor spec §14 suite, `cross=yes` on the signaculum
TEST row), including a cross-phase run proving the f64 pipelines agree
big-endian (commit `181e95f`).

The reference CRC the ROM checks against comes from the expected.out payload,
computed from an Exsecutor checkout at the pinned commit:

```
python3 - <<'EOF'
import zlib, pathlib
p = pathlib.Path('tests/programs/signaculum/expected.out').read_bytes()
assert p[:15] == b'P6\n256 256\n255\n' and len(p) == 196623
print(f"{zlib.crc32(p[-196608:]) & 0xFFFFFFFF:#010x}")
EOF
# -> 0x2025c173   (CRC-32, ISO-HDLC: poly 0xEDB88320, init/xorout 0xFFFFFFFF)
```

In THIS repository the toolchain is a different one again —
`mips64-elf-gcc` 14.4.0 targeting `vr4300`, not qemu-musl-mips64 — and the
VR4300's FPU flushes subnormals where qemu's soft-fp preserves them, so
byte-identity is **[UNTESTED]** until the ROM's in-ROM CRC self-check has run
and printed `signaculum: AGREE`. That check exists precisely to turn this
paragraph from a claim into a measurement.

## Measured with this repo's toolchain

`mips64-elf-gcc` 14.4.0, `-mabi=o64 -march=vr4300 -Os -std=gnu2x
-fno-fast-math -fstack-usage`:

| frame | bytes |
|---|---|
| `exs_signaculum_pingue` | 36,176 |
| `exs_lege_i32` | 112 |
| `exs_lege_u16` | 96 |
| `exs_lege_octeto` | 80 |

`exs_signaculum_pingue`'s frame is dominated by its three locals
`sx`/`sy`/`qq` (`acies<f64, 1493>` each: 35,832 bytes of arrays), which is why
the demo renders on a 49,152-byte kthread rather than the StreamDB reader's
32,768 — 36,176 does not fit in the reader's 32 KB.

The object imports exactly `exsrt_abortus` (supplied by the ROM). Under
`-ffast-math` the unit refuses to compile (`#error "exsecutor: -ffast-math is
refused (spec 5.4)"`); that refusal is load-bearing and the engine Makefile
appends `-fno-fast-math` after n64.mk's global `-ffast-math` for exactly this
reason. Unlike the StreamDB reader it contains real FP opcodes by design —
the rasterizer IS f64 arithmetic.

## Licensing

Exsecutor is GPL-3.0-or-later with a stated exception: **code produced by the
compiler is not covered by the GPL** (Exsecutor's `LICENSE.EXCEPTION`,
Exception A). These generated files therefore carry no GPL obligation and sit
under this repository's MIT licence. The `.exsg` stream is derived from the
project's own logo; see the Exsecutor repo's `logo/` for that provenance.
