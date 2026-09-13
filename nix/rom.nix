# SPDX-License-Identifier: MIT
#
# nix/rom.nix — mkN64Rom: build a libdragon project into a bootable .z64.
#
# ── Why this takes a Makefile instead of generating one ─────────────────
# libdragon's whole build model is `include $(N64_INST)/include/n64.mk` plus a
# handful of variables, and every upstream example, tutorial, and third-party
# engine (Tiny3D, Pyrite64) is shaped that way. Generating Makefiles here would
# mean re-implementing n64.mk's asset rules, DFS packing, ELF compression and
# symbol table generation — and would silently diverge from upstream on every
# libdragon bump. So a project brings its own Makefile, exactly as it would
# outside Nix, and this function supplies the hermetic environment around it:
# a pinned toolchain, a pinned libdragon, and the two prefix variables wired up.
#
# `nix flake init -t <this-flake>#hello` gives you a working Makefile to start
# from.
{ pkgs, n64Inst, toolchain }:

{ name
, src
, version ? "0.1.0"
  # Overrides for n64.mk's ROM header variables. Passed on the make command
  # line, so they beat whatever the project's Makefile sets.
, romTitle ? null # N64_ROM_TITLE — max 20 chars, ASCII; goes in the ROM header
, saveType ? null # none eeprom4k eeprom16 sram256k sram768k sram1m flashram
, regionFree ? null # true -> boots on any console region
  # Derivations that expose a `filesystem/` directory (e.g. mkBakedInstrument
  # outputs). Their contents are merged into the project's `filesystem/` before
  # make runs, so n64.mk's mkdfs rule packs them into the ROM's DragonFS image.
  # Kept as a list of derivations rather than paths so a ROM's asset
  # dependencies are visible in the derivation graph.
, assets ? [ ]
, makeFlags ? [ ]
, nativeBuildInputs ? [ ]
  # The ROM's audio_init sample rate. If non-null, the build cross-checks
  # that every asset in `assets` that exports an audio rate (via
  # nix-support/audio-rate) matches. A mismatch causes pitch/time drift, not
  # silence — a subtle defect that is hard to catch by ear. Baked instruments
  # from mkBakedInstrument export this; mkSound/mkMusic do not (they inherit
  # the rate from their source WAV, which is the ROM author's responsibility).
, audioRate ? null
  # Build the ROM with the on-screen retro debug console wired in (sets
  # KILN_DEBUG=1, which the example's main.c gates `kiln_console_*` calls on).
  # See engine/src/kiln/kiln_console.h and examples/debug-demo. Off by default
  # so non-debug ROMs stay byte-identical.
, debugConsole ? false
, ...
}@args:

let
  lib = pkgs.lib;

  flagValue = prefix:
    let hit = lib.findFirst (f: lib.hasPrefix prefix f) null makeFlags;
    in if hit == null then null else lib.removePrefix prefix hit;
  jump = let j = flagValue "KILN_JUMP="; in if j != null then j else flagValue "FORGE_MODE=";
  jumpSuffix = if jump == null then "" else "-" + lib.toLower (lib.replaceStrings [ "_" ] [ "-" ] jump);
  kilnRecord = {
    kind = if jump == null then "rom" else "jump";
    example = baseNameOf (toString src);
    inherit name romTitle saveType jump debugConsole;
    # A DERIVATION name, which is not always the package attribute (`music-demo`
    # builds a derivation called `music`); mkStudioManifest resolves it.
    base = if jump == null then null else lib.removeSuffix jumpSuffix name;
    platforms = [ "linux" ];
  };

  # n64.mk interpolates these unquoted (`n64tool -t $(N64_ROM_TITLE)`), so a
  # title containing spaces has to carry its own literal double quotes — which
  # is exactly what libdragon's own examples do (`N64_ROM_TITLE = "Audio
  # Player"`). Without them n64tool sees "Kiln" and "Hello" as separate args and
  # fails with "Need output flag before first file".
  romVars =
    lib.optional (romTitle != null) ''N64_ROM_TITLE="${romTitle}"''
    ++ lib.optional (saveType != null) "N64_ROM_SAVETYPE=${saveType}"
    ++ lib.optional (regionFree != null && regionFree) "N64_ROM_REGIONFREE=true";

  passthruArgs = builtins.removeAttrs args [
    "name"
    "src"
    "version"
    "romTitle"
    "saveType"
    "regionFree"
    "assets"
    "makeFlags"
    "nativeBuildInputs"
    "debugConsole"
  ];

  # Caller-supplied makeFlags, with the debug-console flag appended when the
  # ROM opts in. The flag is passed as the make variable KILN_DEBUG=1; the
  # included kiln-inst.mk translates it to -DKILN_DEBUG=1 *after* n64.mk has
  # established N64_CFLAGS, avoiding the command-line precedence trap where
  # passing N64_CFLAGS+=... would override n64.mk's defaults and drop the
  # include paths.
  makeFlagsWithDebug = makeFlags ++ lib.optional debugConsole "KILN_DEBUG=1";

in
pkgs.stdenv.mkDerivation (passthruArgs // {
  pname = name;
  inherit version src;

  nativeBuildInputs = [ toolchain n64Inst pkgs.gnumake ] ++ nativeBuildInputs;

  # See libdragon.nix — hardening is meaningless and actively harmful for a
  # bare-metal ROM. This has to be set on the ROM build too, not just on
  # libdragon's, because it governs the flags our cc-wrapper injects into every
  # cross compile the project's Makefile performs.
  hardeningDisable = [ "all" ];

  # The two variables n64.mk keys off. Keeping them distinct (rather than
  # merging both trees into one prefix) is exactly the case n64.mk's
  # `N64_GCCPREFIX ?= $(N64_INST)` override exists for.
  N64_INST = n64Inst;
  N64_GCCPREFIX = toolchain;

  makeFlags = makeFlagsWithDebug;

  dontConfigure = true;

  # nixpkgs' fixupPhase assumes host ELFs. Left alone it runs `strip` over the
  # MIPS ELF we deliberately keep for symbolised crash backtraces (n64sym /
  # GDB), and `patchelf` noisily fails on it since a ROM binary is statically
  # linked with no .dynamic section. Neither has anything useful to do here.
  dontStrip = true;
  dontPatchELF = true;

  buildPhase = args.buildPhase or ''
    runHook preBuild
${lib.optionalString (assets != [ ]) ''
    mkdir -p filesystem
    for a in ${lib.escapeShellArgs (map toString assets)}; do
      if [ ! -d "$a/filesystem" ]; then
        echo "mkN64Rom: asset $a has no filesystem/ directory" >&2
        exit 1
      fi
      cp -rL --no-preserve=mode "$a"/filesystem/. filesystem/
      # Nix store dirs are 0555; without --no-preserve=mode the first cp
      # leaves filesystem/sfx etc. read-only and a second asset sharing
      # that subdirectory (e.g. two mkSound outputs both under sfx/)
      # fails with "Permission denied". chmod again so the next iteration
      # is robust even if cp's mode handling changes.
      chmod -R u+w filesystem
    done
    echo "assets staged into filesystem/:"
    ls -l filesystem
''}
${lib.optionalString (audioRate != null) ''
    # Cross-check baked instrument rates against the ROM's declared audio rate.
    # A mismatch causes pitch/time drift, not silence.
    for a in ${lib.escapeShellArgs (map toString assets)}; do
      if [ -f "$a/nix-support/audio-rate" ]; then
        rate=$(cat "$a/nix-support/audio-rate")
        if [ "$rate" != "${toString audioRate}" ]; then
          echo "FAIL: asset $a baked at ''${rate} Hz but ROM audioRate is ${toString audioRate} Hz" >&2
          echo "       A mismatch causes pitch/time drift, not silence." >&2
          exit 1
        fi
        echo "  audio rate check: $a (''${rate} Hz) OK"
      fi
    done
''}
    make -j"$NIX_BUILD_CORES" ${lib.escapeShellArgs romVars} ''${makeFlags[@]}

${lib.optionalString (assets != [ ]) ''
    # ── An `assets` list that never reaches the ROM ────────────────────────
    # n64.mk only builds and attaches a DragonFS if the project's Makefile
    # declares one (`$(BUILD_DIR)/<name>.dfs: ...` plus a `<name>.z64:`
    # dependency on it). Without those two lines the `filesystem/` directory
    # this function just populated is IGNORED and the ROM comes out
    # byte-for-byte identical to the assetless build — after which every
    # `rom:/` path fails at runtime.
    #
    # That has now cost two ROMs built on this engine. A downstream game
    # shipped without a filesystem for a while because its Makefile was
    # copied from a game that had no assets, and Forge repeated it exactly:
    # `.#forge-dfs` booted with the level absent and no layer complained,
    # because a missing DFS and an empty one are the same thing to every
    # reader below.
    #
    # So it is a build failure now instead of a runtime mystery. The check is
    # the direct one — did make produce a .dfs — rather than inspecting the
    # ROM, because that is the artifact whose absence IS the bug.
    if [ -n "$(find . -name '*.dfs' -print -quit)" ]; then
      echo "mkN64Rom: DragonFS attached ($(find . -name '*.dfs' | tr '
' ' '))"
    else
      echo "mkN64Rom: ${name} declares ${toString (builtins.length assets)} asset(s)"            "but the build produced no .dfs, so NONE of them are in the ROM." >&2
      echo "  Add these two lines to the project's Makefile (see" >&2
      echo "  Forge/Makefile's 'THE FILESYSTEM' comment):" >&2
      echo "" >&2
      # The ROM's own basename, taken from what make actually emitted rather
      # than from the flake attribute — those differ whenever one project builds
      # several variants (Forge/Makefile emits forge.z64 for both `.#forge` and
      # `.#forge-dfs`), and printing a make target that does not exist sends the
      # reader off to debug the advice instead of the bug.
      shopt -s nullglob
      built=(*.z64)
      base="${name}"
      if [ ''${#built[@]} -gt 0 ]; then base="$(basename "''${built[0]}" .z64)"; fi
      echo "    \$(BUILD_DIR)/$base.dfs: \$(wildcard filesystem/*) \$(wildcard filesystem/*/*)" >&2
      echo "    $base.z64: \$(BUILD_DIR)/$base.dfs" >&2
      exit 1
    fi
''}
    runHook postBuild
  '';

  # A libdragon build leaves the ROM in the project root and the intermediate
  # ELF (which n64sym/GDB need for symbolised backtraces) under build/.
  installPhase = args.installPhase or ''
    runHook preInstall
    mkdir -p $out/{,lib/debug}

    shopt -s nullglob
    roms=(*.z64)
    if [ ''${#roms[@]} -eq 0 ]; then
      echo "mkN64Rom: no .z64 produced — check the project's Makefile target" >&2
      exit 1
    fi
    cp "''${roms[@]}" $out/

    # Keep the ELF + symbol table: without them a crash on hardware gives you
    # raw addresses instead of a backtrace.
    for f in build/*.elf build/*.sym *.elf *.sym; do
      cp "$f" $out/lib/debug/ 2>/dev/null || true
    done
    runHook postInstall
  '';

  passthru = (args.passthru or { }) // {
    inherit n64Inst toolchain;
    romFile = "${placeholder "out"}/${name}.z64";
    # Kiln Studio's project record (lib.mkStudioManifest reads it). Derived
    # from what this call already has, so no ROM's call site changes: the
    # example is the source directory's name, and a jump ROM is recognised by
    # the KILN_JUMP / FORGE_MODE make flag mkJumpRoms and forgeModeRoms pass.
    # passthru is not part of the derivation, so adding a field rebuilds nothing.
    kiln = kilnRecord;
  };

  meta = (args.meta or { }) // {
    platforms = pkgs.lib.platforms.linux;
  };
})
