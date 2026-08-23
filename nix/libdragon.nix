# SPDX-License-Identifier: MIT
#
# nix/libdragon.nix — libdragon, built into a store path usable as `N64_INST`.
#
# libdragon is the SDK the feasibility report selects over libultra: modern GCC,
# the RSP-accelerated 32-channel mixer, wav64 (VADPCM/Opus), XM64/YM64, rspq,
# `debugf()` over flashcart USB, and the host tools — including audioconv64,
# which is the workhorse of the report's Stage 1 (bake instruments offline to
# VADPCM rather than synthesising them on the VR4300).
#
# ── Why this is ONE derivation and not a native/cross pair ──────────────
# libdragon's build produces two kinds of artifact at once: host tools (x86_64
# here) and the target library (mips64). Its Makefiles are built for exactly
# that: the host tools compile with plain `$(CC)`, while the target library goes
# through `$(N64_CC)` = `$(N64_GCCPREFIX)/bin/mips64-elf-gcc`. So this is a
# NATIVE derivation that invokes a cross compiler for part of its work, which
# mirrors upstream's own `build.sh`. Splitting it into two derivations would
# mean fighting the Makefile for no benefit.
#
# ── The resulting layout (from the Makefile's install targets) ──────────
#   $out/include/n64.mk                     the makefile fragment ROMs include
#   $out/mips64-elf/lib/libdragon.a         the target library
#   $out/mips64-elf/lib/libdragonsys.a      syscall/crt layer (replaces libgloss)
#   $out/mips64-elf/lib/{n64,dso,rsp}.ld    linker scripts
#   $out/mips64-elf/include/                headers, *.inc, ucode.S
#   $out/bin/                               n64tool, mkdfs, audioconv64, ...
#
# Note this is NOT merged with the toolchain: n64.mk keeps `N64_GCCPREFIX`
# separate from `N64_INST` precisely so a toolchain installed elsewhere can be
# used, which is exactly our situation. See n64-inst.nix.
{ pkgs, src, toolchain }:

pkgs.stdenv.mkDerivation {
  pname = "libdragon";
  version = "unstable-${builtins.substring 0 7 (src.rev or "dirty")}";
  inherit src;

  nativeBuildInputs = [
    toolchain
    pkgs.gnumake
    pkgs.which
  ];

  # The vendored host-tool sources (FreeType amalgam, plutosvg, dr_wav,
  # libsamplerate) are compiled with -Werror against whatever host GCC nixpkgs
  # currently ships. Upstream cannot have tested every one of those, and a new
  # host-compiler warning is not a reason for our build to fail.
  env.NIX_CFLAGS_COMPILE = "-Wno-error";

  # `-fPIE`, `-fstack-protector`, `_FORTIFY_SOURCE` and friends are all wrong
  # for a bare-metal ROM: there is no dynamic loader, no stack guard, and no
  # libc to fortify against. Disable for both the host tools and, critically,
  # every cross invocation the Makefile makes.
  hardeningDisable = [ "all" ];

  # ── newlib's MIPS `struct stat` is the legacy layout ────────────────────
  # newlib's sys/stat.h has a hard-coded special case:
  #
  #   #elif defined(__mips__) && !defined(__rtems__)
  #     time_t st_atime; long st_spare1;
  #     time_t st_mtime; long st_spare2;   /* scalars, not struct timespec */
  #
  # so on MIPS there is no `st_mtim`/`st_atim`/`st_ctim` timespec member and no
  # `st_mtime`-as-macro. libdragon's FAT layer assumes the modern POSIX layout
  # and fails with "'struct stat' has no member named 'st_mtim'".
  #
  # This is not a feature-test-macro problem — no combination of _POSIX_C_SOURCE
  # or _GNU_SOURCE reaches the timespec branch on this target. Two lines are
  # affected in the whole tree, so rewrite them to the layout newlib actually
  # provides. Sub-second resolution is lost, which is meaningless for FAT
  # timestamps: the on-disk format stores two-second granularity anyway.
  postPatch = ''
    substituteInPlace src/fat.c \
      --replace-fail 'st->st_mtim.tv_sec = mktime(&tm);' 'st->st_mtime = mktime(&tm);' \
      --replace-fail 'st->st_mtim.tv_nsec = 0;' ""

    # ── libxm xm_tick divide-by-zero on a bpm==0 module ───────────────────
    # xm_tick (src/audio/libxm/play.c:1275) computes
    #   ctx->remaining_samples_in_tick += ctx->rate / (ctx->bpm * 0.4)
    # with no guard. A malformed XM module (missing or zeroed BPM field —
    # the cinematic's examples/music/test.xm ships with both `tempo` and
    # `bpm` zeroed in the header) will raise a floating-point divide-by-zero
    # exception on the VR4300 when the XM is tick'd. Park the accumulator at
    # 0 and continue so the rest of the audio path (any .wav64 SFX) keeps
    # producing samples instead of crashing the whole mixer. The XM itself
    # stays silent — which a zero-BPM module should be anyway.
    substituteInPlace src/audio/libxm/play.c \
      --replace-fail \
        'ctx->remaining_samples_in_tick += (float)ctx->rate / ((float)ctx->bpm * 0.4f);' \
        'if(ctx->bpm > 0) { ctx->remaining_samples_in_tick += (float)ctx->rate / ((float)ctx->bpm * 0.4f); }'
  '';

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    # libdragon installs into $N64_INST as it builds (install-mk runs before
    # the library so that n64.mk is available to sub-makes), so $out has to
    # exist and be the install prefix from the start.
    export N64_INST="$out"
    export N64_GCCPREFIX="${toolchain}"
    mkdir -p "$out"/{bin,include} "$out"/mips64-elf/{lib,include}

    make -j"$NIX_BUILD_CORES" install-mk
    make -j"$NIX_BUILD_CORES" libdragon
    make -j"$NIX_BUILD_CORES" tools

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall

    export N64_INST="$out"
    export N64_GCCPREFIX="${toolchain}"
    make install
    make tools-install

    runHook postInstall
  '';

  # Fail here rather than three layers deep in a ROM build if upstream moves
  # something. Every path below is one a downstream consumer hard-codes.
  doInstallCheck = true;
  installCheckPhase = ''
    for f in include/n64.mk \
             mips64-elf/lib/libdragon.a \
             mips64-elf/lib/libdragonsys.a \
             mips64-elf/lib/n64.ld \
             mips64-elf/include/libdragon.h \
             bin/n64tool \
             bin/mkdfs \
             bin/audioconv64 \
             bin/n64sym \
             bin/n64elfcompress; do
      if [ ! -e "$out/$f" ]; then
        echo "libdragon.nix: expected $f in the install tree — upstream layout changed" >&2
        exit 1
      fi
    done
  '';

  passthru = { inherit toolchain; };

  meta = {
    description = "libdragon — open-source Nintendo 64 SDK (library + host tools)";
    homepage = "https://libdragon.dev";
    license = pkgs.lib.licenses.unlicense;
    platforms = pkgs.lib.platforms.linux;
  };
}
