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

## `fig_pose_*.gen.c` — the bone arithmetic

Generated C. **Never hand-edit either file**; regenerate them. Unlike
`signaculum_*`, whose source is an Exsecutor EXAMPLE, this one is Figulina's
own: [`engine/src/kiln/fig_pose.exsc`](../fig_pose.exsc) lives in this repo
and is the first engine arithmetic written once and emitted per row.

| | x86_64 variant | mips64 variant |
|---|---|---|
| source | `engine/src/kiln/fig_pose.exsc` | same |
| compiler | `exsc` at Exsecutor commit `b77ad1ffef4748d37728e63d75a3f3ec491eb8d1` (503,761 bytes, sha256 `02a16a5091eed50b9f86a88669c89835df525fba2e956523dc4b8705402d0d9c`) | same |
| row | `--hospes x86_64-linux --emitte c` | `--hospes mips64-none-o64 --emitte c` |
| bytes | 28,703 | 28,720 |
| sha256 | `333d6326c85625c817c46a7d7d3b4773d460dfd0232161ce535edb00de35126b` | `01970224d859cf2c4bc0b150f68bd1c5be612ba6d86109e47111faa5894021d1` |

The two emissions differ in **exactly three lines**, and every one is a row
fact rather than a compiler mood: `_Static_assert(sizeof(void *) == 8)`
against `== 4`; one `+` gaining an `exsi_norm_u(…, 32)`; and one
`exsi_mul_u(…, 64)` becoming `…, 32)`. That is `mensura` narrowing to 32 bits
on the console row, said three times in three places that must agree.

### What it contains, and what it deliberately does not

`exs_fig_quat_mul` (Hamilton product) and `exs_fig_pose_subtree_mask`. Both
pure, on caller-owned buffers — no allocation, no strings, no `dyn`. That is
not a style choice: `backend_c/emit_c.inc`'s `__bfc_refuse` declines
`retain`/`release` by name ("no object header exists in library mode"), so a
program that allocates cannot be emitted as C at all, and `--emitte c` is the
ONLY backend for the `mips64-none-o64` row. Anything the console runs must be
allocation-free.

Three of `kiln_pose.c`'s five functions are **absent and cannot be added
today**, for one reason rather than three: Exsecutor's float surface is
eleven IR opcodes (`FADD FCMP FCONST FDIV FEXT FMA FMUL FNEG FSUB FTOI
FTRUNC`) with no root and no transcendental. `fig_quat_nlerp` normalises by
`1/sqrtf`, `fig_pose_blend_masked` calls it, and `fig_quat_axis_angle` needs
`fm_sinf`/`fm_cosf`. A software square root is not a way round it:
`kiln_pose.h`'s invariant is that a masked blend and an unmasked one agree
about what halfway means, and the unmasked one is Tiny3D's `t3d_quat_nlerp`
calling `sqrtf` — an approximation that is only self-consistent breaks
exactly that agreement. `kiln_pose.c` therefore stays, and this core sits
beside it rather than replacing it.

### Evidence

`nix/checks/fig-pose-parity.nix` compiles the committed x86_64 emission next
to `engine/src/kiln/exsc/fig_pose_parity.c` and compares against a
transcription of `kiln_pose.c`'s arithmetic — 20,000 `quat_mul` cases by
`memcmp` (zero tolerance: no division, no root, nothing a compiler may
reassociate) and 100,000 mask cases over depth-first columns. Run four ways
under `-fsanitize=undefined -fno-sanitize-recover=all`, gcc and clang at
`-O0` and `-O2`, all four stdouts byte-identical. The check needs **no
`exsc`**, which is what keeps it cheap and matches how this repo already
treats `signaculum_*`.

The mips64 emission is verified to COMPILE for the console row (`ELF 32-bit
MSB relocatable, MIPS, N32 MIPS-III`, exporting exactly the two functions and
importing exactly `exsrt_abortus`). It is **not** verified behaviourally:
no qemu run was made, so big-endian bit-identity is unmeasured.

## Licensing

Exsecutor is GPL-3.0-or-later with a stated exception: **code produced by the
compiler is not covered by the GPL** (Exsecutor's `LICENSE.EXCEPTION`,
Exception A). These generated files therefore carry no GPL obligation and sit
under this repository's MIT licence. The `.exsg` stream is derived from the
project's own logo; see the Exsecutor repo's `logo/` for that provenance.
