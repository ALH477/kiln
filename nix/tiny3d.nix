# SPDX-License-Identifier: MIT
#
# nix/tiny3d.nix — Tiny3D, the N64 3D library layered on libdragon.
#
# Report §7 names Tiny3D (and its RSPL microcode) as the model to study for
# writing custom RSP code, and Tiny3D is what the modern N64 homebrew 3D stack
# is actually built on. Here it provides the 3D half of the engine; libdragon's
# rdpq provides the 2D half (see examples/engine).
#
# ── Why this needs a MERGED prefix ──────────────────────────────────────
# Tiny3D's `install` target writes into `$(N64_INST)` — the same prefix
# libdragon occupies — and its `t3d-inst.mk` fragment does
# `N64_LDFLAGS := -lt3d $(N64_LDFLAGS)`, expecting `libt3d.a` to sit in
# libdragon's own lib directory. Nix store paths are immutable, so instead of
# installing into libdragon we install into our own $out with the same layout
# and let n64-inst.nix symlinkJoin the two into one prefix. That is the case
# the plan's `n64Inst` idea was actually for.
#
# The RSP microcode `.S` files are checked into the repo next to their `.rspl`
# sources, so the RSPL transpiler is NOT a build dependency — it is only needed
# to modify the microcode.
{ pkgs, src, toolchain, libdragon }:

pkgs.stdenv.mkDerivation {
  pname = "tiny3d";
  version = "unstable-${builtins.substring 0 7 (src.rev or "dirty")}";
  inherit src;

  # The one vendored Tiny3D patch. Pure refactor: extracts the body of
  # t3d_model_load(path) into t3d_model_load_buf(buf, sz) so kiln_asset_model
  # can parse a .t3dm already in RAM (out of a StreamDB container). See
  # nix/patches/tiny3d-load-buf.patch for the rationale. Upstream-able as-is;
  # if Tiny3D bumps and this no longer applies, `nix flake check` fails loudly
  # here rather than silently at runtime.
  patches = [ ./patches/tiny3d-load-buf.patch ];

  nativeBuildInputs = [ toolchain libdragon pkgs.gnumake pkgs.which ];

  # Bare metal — see nix/libdragon.nix.
  hardeningDisable = [ "all" ];

  # Tiny3D compiles with -Wall -Wextra -Werror. Against our pinned libdragon +
  # GCC 14 that fails immediately, and not on Tiny3D's own code: libdragon's
  # headers declare `volatile int audio_can_write()` and similar, which trips
  # -Wignored-qualifiers. Those headers arrive through a plain -I, so they are
  # not treated as system headers and their warnings are not suppressed.
  #
  # Patching the flag out of the Makefile is deliberate rather than setting
  # NIX_CFLAGS_COMPILE: that variable is consumed by the cc-wrapper under a
  # role-salted name, and which name applies to a cross compiler sitting in a
  # native derivation's nativeBuildInputs is exactly the kind of thing that
  # silently stops working. Editing the flag we can see is unambiguous.
  # Note this substitutes -Wno-error IN rather than merely deleting -Werror:
  # libdragon's own n64.mk puts `-Wall -Werror` into N64_C_AND_CXX_FLAGS, which
  # lands *earlier* in the command line than anything Tiny3D appends. Deleting
  # Tiny3D's own -Werror is therefore not enough — a warning from libdragon's
  # headers (e.g. a %d/enum format mismatch in magma.h's assertf) still fails
  # the build. A trailing -Wno-error wins over the earlier -Werror.
  postPatch = ''
    substituteInPlace Makefile \
      --replace-fail '-Wall -Wextra -Werror' '-Wall -Wextra -Wno-error'
  '';

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    # N64_INST points at libdragon (so `include $(N64_INST)/include/n64.mk`
    # resolves), while INSTALLDIR is overridden on the command line to our own
    # output. The Makefile sets `INSTALLDIR=$(N64_INST)` with a plain `=`, so a
    # command-line assignment wins.
    export N64_INST="${libdragon}"
    export N64_GCCPREFIX="${toolchain}"

    make -j"$NIX_BUILD_CORES"                       # libt3d.a (+ RSP ucode)
    make -j"$NIX_BUILD_CORES" -C tools/gltf_importer # gltf_to_t3d host tool

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    export N64_INST="${libdragon}"
    export N64_GCCPREFIX="${toolchain}"
    mkdir -p $out/{bin,include} $out/mips64-elf/{lib,include/t3d}

    make install INSTALLDIR="$out"
    make -C tools/gltf_importer install INSTALLDIR="$out"

    runHook postInstall
  '';

  # libt3d.a is a MIPS archive and gltf_to_t3d is a host binary; nixpkgs' fixup
  # tooling should only touch the latter.
  dontStrip = true;
  dontPatchELF = true;

  doInstallCheck = true;
  installCheckPhase = ''
    for f in include/t3d.mk \
             mips64-elf/lib/libt3d.a \
             mips64-elf/include/t3d/t3d.h \
             mips64-elf/include/t3d/t3dmath.h \
             mips64-elf/include/t3d/t3dmodel.h \
             bin/gltf_to_t3d; do
      if [ ! -e "$out/$f" ]; then
        echo "tiny3d.nix: expected $f — upstream layout changed" >&2
        exit 1
      fi
    done
  '';

  meta = {
    description = "Tiny3D — a small 3D library for the Nintendo 64, on libdragon";
    homepage = "https://github.com/HailToDodongo/tiny3d";
    license = pkgs.lib.licenses.mit;
    platforms = pkgs.lib.platforms.linux;
  };
}
