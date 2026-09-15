# exsec-signaculum-demo

The Exsecutor logo, rasterized on the VR4300 by code the Exsecutor compiler
emitted — Kiln's built-in software 3D path (`kiln_soft3d`, the engine
module wrapping `exs_signaculum_pingue`) — and **checked in-ROM**: a CRC-32
of the 196,608-byte framebuffer against the constant baked from Exsecutor's
own `expected.out` (see `main.c` and
`../../engine/src/kiln/gen/PROVENANCE.md` for every pin and command).

The screen: the logo's frame, letterboxed 32 px each side into 320x240, a
status strip with the render code and the two CRCs, and the render thread's
measured stack high-water mark against the static 36,176-byte
`-fstack-usage` figure.

The verdict line is on debugf (ISViewer): `signaculum: AGREE ...` or
`signaculum: DISAGREE ...`.

## Build and run (until the flake wiring lands)

```sh
nix develop                                   # sets N64_INST + N64_GCCPREFIX
make -C ../../engine all                      # libkiln.a incl. kiln_soft3d.o
                                              # (`all` named: the default goal
                                              #  is print-modules, a flake
                                              #  interface, not the archive)
cd examples/exsec-signaculum-demo && make     # -> exsec-signaculum-demo.z64
```

Then either deploy (`./dev deploy exsec-signaculum-demo` from the repo root)
or boot in Ares with ISViewer's stdout visible:

```sh
rom=examples/exsec-signaculum-demo/exsec-signaculum-demo.z64
work=$(mktemp -d); cp "$rom" "$work/rom.z64"; chmod u+w "$work/rom.z64"
nix run .#ares -- --system "Nintendo 64" --no-file-prompt --kiosk \
    --settings-file "$work/settings.bml" "$work/rom.z64" 2>&1 | tee "$work/ares.log"
grep -a "signaculum:" "$work/ares.log"
```

or capture the window: `./dev shot result/exsec-signaculum-demo.z64 out.png`
once the flake entry (below) exists — `nix/demos/exsec-signaculum-demo.nix`
in this directory is the DRAFT of that entry, deliberately not wired.

## What AGREE certifies

The same framebuffer bytes the Exsecutor repo certifies on x86-64 and under
qemu-musl-mips64 (its signaculum TEST row, `cross=yes`), reproduced here by
`mips64-elf-gcc` 14.4.0 compiling the `mips64-none-o64` row of the C backend
and running on the VR4300's real FPU — a third toolchain and the first
hardware target. DISAGREE with a matching `status=0` would mean the FP
pipelines diverged (the VR4300 flushes subnormals where qemu's soft-fp
preserves them); DISAGREE with `status!=0` means the stream on this ROM is
not the pinned blob.
