# SPDX-License-Identifier: MIT
#
# nix/host.nix — the host tier, built once, for any toolchain.
#
# ── Why this file exists ───────────────────────────────────────────────
# Seven checks each carried their own copy of the same gcc line: the same
# -std=gnu2x -Wall -Wextra -Werror, the same three -I paths, the same
# $(echo plat/host/src/*.c) glob, the same libkilnmath.a. That was survivable
# while there was one compiler. It is not survivable with four, because a flag
# that has to be identical everywhere — and there is now exactly such a flag,
# see -ffp-contract below — would have to be added in seven places and would
# be missing from the eighth the day someone adds a check.
#
# So the compile recipe lives here, once, and a target is a set of answers to
# "which compiler, which flags, how do I run the result". The checks say what
# to build and what to compare; they no longer say how to build it.
#
# ── Why -ffp-contract=off, and what it does NOT currently do ───────────
# Six checks compare a rendered PNG byte for byte, and GCC and Clang both
# default to -ffp-contract=fast in GNU C modes, which lets `a*b + c*d` fuse
# into an FMA wherever the target has one. host_t3d.c's edge function is
# exactly `(px-ax)*(by-ay) - (py-ay)*(bx-ax)`, and its sign decides whether a
# pixel is inside a triangle, so a fused multiply-add there can move the edge
# of every triangle on the screen.
#
# MEASURED, because the first version of this comment asserted the flag was
# what made the multi-architecture claim work and that was not true:
#
#   aarch64, -O1, default          0 FMA instructions in host_t3d.s
#   aarch64, -O1, -ffp-contract=off  0
#   aarch64, -O2, default         74
#   aarch64, -O2, -ffp-contract=off  0
#
# The gates compile at -O1, where GCC contracts nothing on its own, and
# aarch64 and riscv64 reproduce the x86_64 reference PNGs with the flag
# REMOVED. So today this flag changes no output on any target. It is kept
# because the mechanism is real and one -O away: raise the optimisation level
# for speed, or take a compiler that contracts more eagerly at -O1, and every
# triangle edge moves against a committed reference for a reason nobody would
# look for in a build flag.
#
# It still belongs here rather than in seven checks, for the reason above:
# a flag that must be identical everywhere cannot live in seven places.
# ── What a target is not ───────────────────────────────────────────────
# Not a platform abstraction inside the engine. There is still no #ifdef in
# any engine .c and no second renderer: every target compiles the identical
# sources through plat/host/include's <libdragon.h>. A target is a compiler
# and a way to run a binary, nothing more.
{ pkgs, lib ? pkgs.lib, libdragonSrc, engineSrc, platHost, platShell
, streamdbSrc, webShellHtml }:

let
  # ── the flags, in one place ────────────────────────────────────────
  # -Werror on purpose: engine/Makefile ships -Wno-error for third-party
  # header noise on the cross build, so the host tier is where the engine
  # gets a strict second opinion. Four compilers' worth of it, now.
  baseCFlags = [
    "-O1" "-g" "-std=gnu2x"
    "-Wall" "-Wextra" "-Werror" "-Wno-unused-parameter"
    "-ffp-contract=off"
  ];

  # kiln_asset.c includes <streamdb/streamdb_embedded.h>, i.e. through a
  # directory the N64 prefix creates on install and a source checkout does
  # not. Staging it here rather than teaching the engine a second spelling
  # keeps the include identical on both targets.
  streamdbInc = pkgs.runCommand "streamdb-emb-inc-host" { } (
    "mkdir -p $out/include/streamdb\n"
    + "cp ${streamdbSrc}/include/streamdb_embedded.h $out/include/streamdb/\n");

  # The two files plat/host does not implement but every host program needs:
  # StreamDB's reader and its DFS backend, over the shim's dfs_*.
  streamdbSources = [
    "${streamdbSrc}/src/streamdb_embedded.c"
    "${streamdbSrc}/src/streamdb_io_dfs.c"
  ];

  # ── zlib, built by the target's own compiler ───────────────────────
  # host_png.c is the one third-party dependency in the backend, and every
  # obvious way to satisfy it is architecture-specific: pkgs.zlib is the build
  # machine's, crossPkgs.zlib.static is a second spelling per cross target,
  # and emcc's -sUSE_ZLIB=1 DOWNLOADS the port, which a Nix sandbox has no
  # network for. Compiling the same source with the same compiler as
  # everything else is the only answer that is one answer.
  zlibFor = { name, cc, ar, nativeBuildInputs, preBuild, cflags }:
    pkgs.runCommand "kiln-host-zlib-${name}"
      { inherit nativeBuildInputs;
        src = pkgs.zlib.src;
        meta.description = "zlib for ${name}, compiled by the host target"; }
      ''
        set -euo pipefail
        ${preBuild}
        tar xf $src && cd zlib-*
        for f in adler32.c compress.c crc32.c deflate.c gzclose.c gzlib.c \
                 gzread.c gzwrite.c infback.c inffast.c inflate.c inftrees.c \
                 trees.c uncompr.c zutil.c; do
          ${cc} -O2 -DHAVE_UNISTD_H -DHAVE_STDARG_H ${cflags} -c "$f" -o "''${f%.c}.o"
        done
        ${ar} rcs libz.a *.o
        mkdir -p $out/lib $out/include
        cp libz.a $out/lib/
        cp zlib.h zconf.h $out/include/
      '';

  # ── VADPCM, decoded by the decoder that belongs to the encoder ─────
  # Every .wav64 in this repo was produced by audioconv64, which libdragon
  # builds from Dietrich Epp's vendored VADPCM codec (MPL-2.0). On console the
  # decode is RSP microcode with no C fallback, so the obvious host move is to
  # write a decoder from the format description — and nix/host-math.nix's
  # header is a long argument against exactly that: a reimplementation that
  # differs in the last bit makes the host disagree with the console for
  # reasons that have nothing to do with the code under test.
  #
  # decode.c and error.c are 100 lines between them and depend on nothing.
  # Compiled from the pinned libdragon input, not vendored into this tree, so
  # the MPL files stay where they are. See THIRD_PARTY_LICENSES.md.
  #
  # No -Werror: this is third-party source and engine/Makefile already sets
  # the precedent for not holding it to the engine's own bar.
  vadpcmFor = { name, cc, ar, nativeBuildInputs, preBuild, cflags }:
    pkgs.runCommand "kiln-host-vadpcm-${name}"
      { inherit nativeBuildInputs;
        meta.description = "libdragon's VADPCM decoder for ${name}"; }
      (let dir = "${libdragonSrc}/tools/audioconv64/vadpcm"; in ''
        set -euo pipefail
        ${preBuild}
        mkdir -p obj && cd obj
        for f in decode error; do
          ${cc} -O2 -std=gnu11 ${cflags} -I${dir} -c "${dir}/codec/$f.c" -o "$f.o"
        done
        ${ar} rcs libvadpcm.a decode.o error.o
        mkdir -p $out/lib $out/include/codec
        cp libvadpcm.a $out/lib/
        cp ${dir}/codec/vadpcm.h $out/include/codec/
      '');

  mkTarget =
    { name                      # target id, e.g. "native", "wasm32"
    , cc, ar                    # the compiler and archiver commands
    , nativeBuildInputs ? [ ]
    , buildInputs ? [ ]
    , cflags  ? [ ]             # target-specific compile flags
    , ldflags ? [ ]             # target-specific link flags
      # Link flags for CHECK binaries only. Emscripten's NODERAWFS gives a
      # check the real filesystem and the real argv, which is exactly what a
      # harness that writes out.png needs and exactly what a browser build
      # must not have — it is mutually exclusive with --preload-file.
    , checkLdflags ? [ ]
    , preBuild ? ""             # anything the compiler needs before first use
    , run ? ""                  # how to execute a built binary ("" = directly)
    , exe ? ""                  # executable suffix (".js" for Emscripten)
    , description ? name
      # A launcher, if this target has one. null means the target builds and
      # gates but cannot show a window — which is the honest answer for the
      # cross pair: they exist to prove the renderer agrees under qemu, and
      # SDL2 cross-compiled against musl for that purpose would be a large
      # dependency bought for nothing.
    , shell ? null
    }:
    let
      cflagsStr = lib.concatStringsSep " " (baseCFlags ++ cflags);
      ldflagsStr = lib.concatStringsSep " " ldflags;

      zlib = zlibFor {
        inherit name cc ar nativeBuildInputs preBuild;
        cflags = lib.concatStringsSep " " cflags;
      };

      vadpcm = vadpcmFor {
        inherit name cc ar nativeBuildInputs preBuild;
        cflags = lib.concatStringsSep " " cflags;
      };

      hostMath = import ./host-math.nix {
        inherit pkgs cc ar nativeBuildInputs preBuild;
        src = libdragonSrc;
        pname = "kiln-host-math-${name}";
      };

      # Include order is load-bearing: platHost first, so <libdragon.h> and
      # <t3d/*.h> resolve to the shim and not to anything a cross sysroot
      # might happen to carry.
      incs = lib.concatStringsSep " " [
        "-I${platHost}/include"
        "-I${platHost}/src"
        "-I${hostMath}/include"
        "-I${zlib}/include"
        "-I${vadpcm}/include"
        "-I${streamdbInc}/include"
        "-I${streamdbSrc}/include"
        "-I${engineSrc}/src"
        "-I${engineSrc}/src/kiln"
        "-DSTREAMDB_EMB_BACKEND_DFS=1"
      ];

      # ── the backend, as one archive ────────────────────────────────
      # Globbed, not listed. plat/host/src/host_internal.h records why: a
      # third hand-maintained list of these files broke the moment host_tex.c
      # arrived, which is the same drift engine/modules.mk exists to prevent.
      backend = pkgs.runCommand "kiln-host-backend-${name}"
        { inherit nativeBuildInputs buildInputs;
          meta.description = "plat/host, compiled for ${description}"; }
        ''
          set -euo pipefail
          ${preBuild}
          mkdir -p obj && cd obj
          for f in ${platHost}/src/*.c; do
            ${cc} ${cflagsStr} ${incs} -c "$f" -o "$(basename "$f" .c).o"
          done
          for f in ${lib.concatStringsSep " " streamdbSources}; do
            ${cc} ${cflagsStr} ${incs} -c "$f" -o "$(basename "$f" .c).o"
          done
          ${ar} rcs libkilnhost.a *.o
          mkdir -p $out/lib && cp libkilnhost.a $out/lib/
        '';

      # ── the engine's host tier, as one archive ─────────────────────
      # Driven off engine/modules.mk's HOST_MODULES rather than a list here.
      # nix/checks/kiln-parity.nix already holds that list exact in both
      # directions, so this archive is exactly the set of modules the project
      # claims compiles natively — and if the claim is wrong, this fails to
      # build before parity even gets a chance to explain why.
      engine = pkgs.runCommand "kiln-host-engine-${name}"
        { nativeBuildInputs = nativeBuildInputs ++ [ pkgs.gnumake ]; inherit buildInputs;
          meta.description = "libkiln's host modules, compiled for ${description}"; }
        ''
          set -euo pipefail
          ${preBuild}
          cp -r ${engineSrc} engine && chmod -R u+w engine
          mkdir -p obj && cd obj
          for m in $(make -s -C ../engine -f modules.mk print-host-modules); do
            ${cc} ${cflagsStr} ${incs} -c "../engine/src/kiln/$m.c" -o "$m.o"
          done
          ${ar} rcs libkiln.a *.o
          mkdir -p $out/lib && cp libkiln.a $out/lib/
        '';

      libsLine = "${engine}/lib/libkiln.a ${backend}/lib/libkilnhost.a "
               + "${hostMath}/lib/libkilnmath.a ${zlib}/lib/libz.a "
               + "${vadpcm}/lib/libvadpcm.a";

      # ── a host program: check harness, launcher, anything ──────────
      # `sources` are compiled and linked against all three archives. Archive
      # order matters and is fixed here so no caller has to know it: engine
      # first (it calls the backend), backend next (it calls the math), math
      # last.
      mkProgram = { pname, sources, extraCFlags ? [ ], extraLDFlags ? [ ]
                  , extraBuildInputs ? [ ], suffix ? exe, meta ? { } }:
        pkgs.runCommand "${pname}-${name}"
          { nativeBuildInputs = nativeBuildInputs;
            buildInputs = buildInputs ++ extraBuildInputs;
            inherit meta; }
          ''
            set -euo pipefail
            ${preBuild}
            mkdir -p $out/bin
            ${cc} ${cflagsStr} ${lib.concatStringsSep " " extraCFlags} ${incs} \
                  -o $out/bin/${pname}${suffix} \
                  ${lib.concatStringsSep " " sources} \
                  ${libsLine} \
                  ${ldflagsStr} ${lib.concatStringsSep " " extraLDFlags}
          '';

      # ── a host check: build, run, then the caller's comparison ─────
      # `script` runs in a directory holding whatever the program wrote, with
      # $RUN naming how to invoke a binary on this target (empty natively,
      # `node` under Emscripten) — so a check body is identical for every
      # architecture and the reference files are shared, not per-arch.
      mkCheck = { pname, sources, args ? "", preRun ? "", env ? "", script ? ""
                , extraCFlags ? [ ], extraLDFlags ? [ ]
                , extraNativeBuildInputs ? [ ], extraBuildInputs ? [ ]
                , meta ? { } }:
        let prog = mkProgram {
              inherit pname sources extraCFlags extraBuildInputs;
              extraLDFlags = extraLDFlags ++ checkLdflags;
            };
        in pkgs.runCommand "check-${pname}-${name}"
          { nativeBuildInputs = nativeBuildInputs ++ extraNativeBuildInputs
                                ++ [ pkgs.diffutils pkgs.coreutils ];
            buildInputs = buildInputs ++ extraBuildInputs;
            inherit meta; }
          ''
            set -euo pipefail
            mkdir -p work && cd work
            RUN="${run}"
            ${preRun}
            ${env} $RUN ${prog}/bin/${pname}${exe} ${args}
            ${script}
            mkdir -p $out && cp -r . $out/ 2>/dev/null || true
          '';
      # ── a playable build of a game ─────────────────────────────────
      # The game's own main.c is compiled unedited, with -Dmain=kiln_game_main
      # so that a ROM's `int main(void)` stays exactly that and the launcher
      # owns the process entry point. No example has a host branch in it and
      # none was touched to make this work.
      #
      # `assets` is the SAME list of asset derivations mkN64Rom takes, merged
      # the same way and for the same reason (nix/rom.nix's comment on store
      # directory modes) — so a PC or browser build reads the identical
      # filesystem the ROM bakes, rather than a second copy that can drift.
      mkGame = { pname, sources, assets ? [ ], extraCFlags ? [ ], meta ? { } }:
        assert lib.assertMsg (shell != null)
          "nix/host.nix: target ${name} has no launcher; it is a gate target only";
        let
          fs = pkgs.runCommand "${pname}-filesystem" { } (''
            mkdir -p $out/filesystem
          '' + lib.concatMapStrings (a: ''
            if [ ! -d "${a}/filesystem" ]; then
              echo "mkGame: asset ${a} has no filesystem/ directory" >&2; exit 1
            fi
            cp -rL --no-preserve=mode "${a}"/filesystem/. $out/filesystem/
            chmod -R u+w $out/filesystem
          '') assets);
        in
        (mkProgram {
          inherit pname meta;
          suffix = shell.exe or exe;
          sources = sources ++ [
            "${platShell}/kiln_shell_common.c"
            "${platShell}/${shell.source}"
          ];
          extraCFlags = [ "-Dmain=kiln_game_main" "-I${platShell}" ]
                        ++ shell.cflags ++ extraCFlags;
          # A game with no assets gets no filesystem: emcc's file_packager
          # fails outright on an empty preload, and a native build has nothing
          # to point KILN_HOST_DFS at either.
          extraLDFlags = shell.ldflags
                         ++ (if assets == [ ] then [ ] else shell.assetFlags fs);
          extraBuildInputs = shell.buildInputs;
        }) // {
          # Kiln Studio's project record (see nix/rom.nix's). The example is the
          # directory of the game's first source file; the jump is the
          # -DKILN_JUMP a jump variant is compiled with. Attached to the value,
          # not the derivation, so no host build's hash moves.
          kiln = {
            kind = if lib.hasSuffix ".html" (shell.exe or exe) then "web" else "pc";
            target = name;
            example = baseNameOf (dirOf (toString (builtins.head sources)));
            jump =
              let hit = lib.findFirst (f: lib.hasPrefix "-DKILN_JUMP=JUMP_" f) null extraCFlags;
              in if hit == null then null else lib.removePrefix "-DKILN_JUMP=JUMP_" hit;
          };
        };
    in
    {
      inherit name cc ar cflagsStr ldflagsStr incs preBuild run exe shell
              nativeBuildInputs buildInputs hostMath backend engine libsLine zlib vadpcm
              mkProgram mkCheck mkGame description;
    };

  # ── the targets ────────────────────────────────────────────────────
  # native is what nix flake check runs today. wasm32 is gated alongside it
  # because it is the cheap architecture that disagrees with x86_64 about the
  # most things at once: 32-bit pointers, a different libm, no execinfo.h, a
  # different backend in the compiler. aarch64 and riscv64 are real targets
  # and are NOT in the gate set — they are `nix build .#host-arch-aarch64`,
  # because a full cross toolchain is a 130 MB fetch that a pre-push gate has
  # no business pulling. Every one of them fetches from cache; none is built.
  crossTarget = { name, crossPkgs, qemuBin, description }:
    mkTarget {
      inherit name description;
      cc = "${crossPkgs.stdenv.cc}/bin/${crossPkgs.stdenv.cc.targetPrefix}cc";
      ar = "${crossPkgs.stdenv.cc.bintools}/bin/${crossPkgs.stdenv.cc.targetPrefix}ar";
      nativeBuildInputs = [ crossPkgs.stdenv.cc pkgs.qemu-user ];
      # Static, because qemu-user with a dynamic binary wants the cross loader
      # and its library path threaded through, and a software rasteriser has
      # no reason to be dynamically linked inside a sandbox.
      #
      # musl and not glibc, and that is not only about which one links static
      # cleanly: musl has no <execinfo.h>. host_panic.c included it
      # unconditionally until this work, so these two targets are the only
      # thing in the tree that would notice if that guard came back off.
      ldflags = [ "-static" "-lm" ];
      run = "${pkgs.qemu-user}/bin/${qemuBin}";
    };

  # The browser launcher, linked for node instead of for a page. Same C, same
  # EM_JS, same ASYNCIFY loop — the only difference is that emcc emits a .js
  # rather than an .html, so a check can run it. nix/checks/kiln-web.nix pairs
  # it with a DOM stub, which is how the canvas path gets exercised in a
  # sandbox with no display and no browser.
  # emcc writes to its cache on first use, and the store copy is read-only.
  emPreBuild = ''
    export EM_CACHE="$TMPDIR/emcache"
    mkdir -p "$EM_CACHE"
    cp -r ${pkgs.emscripten}/share/emscripten/cache/* "$EM_CACHE"/ 2>/dev/null || true
    chmod -R u+w "$EM_CACHE"
  '';

  webShellFor = ldflags: {
    source = "shell_web.c";
    exe = ".js";
    cflags = [ ];
    inherit ldflags;
    buildInputs = [ ];
    assetFlags = fs: [ "--preload-file" "${fs}/filesystem@/assets" ];
  };

  webShellLdflags = [
    "-sASYNCIFY=1"
    "-sASYNCIFY_STACK_SIZE=65536"
    "-sEXPORTED_RUNTIME_METHODS=['callMain','UTF8ToString','HEAPU8','HEAP16']"
    "-sFORCE_FILESYSTEM=1"
  ];

  targets = {
    native = mkTarget {
      name = "native";
      description = "the build machine (${pkgs.stdenv.hostPlatform.system})";
      cc = "gcc";
      ar = "ar";
      nativeBuildInputs = [ pkgs.gcc pkgs.binutils pkgs.pkg-config ];
      ldflags = [ "-lm" ];
      shell = {
        source = "shell_sdl.c";
        cflags = [ "-I${pkgs.SDL2.dev}/include" "-I${pkgs.SDL2.dev}/include/SDL2" ];
        ldflags = [ "-L${lib.getLib pkgs.SDL2}/lib" "-lSDL2" ];
        buildInputs = [ pkgs.SDL2 ];
        # Native reads the asset directory at run time, so the path is baked
        # into the binary as a default rather than into the executable image.
        # Single quotes around the double quotes: these flags are pasted into
        # a shell command line, so an unquoted string literal loses its quotes
        # to word splitting and the define becomes a division by a path.
        assetFlags = fs: [ "-DKILN_SHELL_DEFAULT_DFS='\"${fs}/filesystem\"'" ];
      };
    };

    # Emscripten. NODERAWFS gives the program the real filesystem and the real
    # argv, so a check harness that writes out.png and a launcher that reads
    # $KILN_HOST_DFS both work unchanged — the browser build swaps it for a
    # preloaded MEMFS, which is a link-flag difference and not a code one.
    wasm32 = mkTarget {
      name = "wasm32";
      description = "wasm32 via Emscripten";
      cc = "emcc";
      ar = "emar";
      nativeBuildInputs = [ pkgs.emscripten pkgs.nodejs ];
      preBuild = emPreBuild;
      ldflags = [
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=64MB"
        "-sSTACK_SIZE=4MB"
        "-lm"
      ];
      checkLdflags = [ "-sNODERAWFS=1" ];
      exe = ".js";
      run = "node";
      # ASYNCIFY is what makes the ROM's own blocking for(;;) legal in a
      # browser — see shell_web.c's header for why inverting the loop was the
      # worse option. .html and not .js because emcc only emits the page, and
      # only accepts --shell-file, when the output is html; the check binaries
      # stay .js and run under node.
      shell = webShellFor (webShellLdflags ++ [ "--shell-file" "${webShellHtml}" ])
              // { exe = ".html"; };
    };
  } // {
    # Identical to wasm32 except that the launcher links as a .js and gets
    # NODERAWFS, so --shot writes a real file. It exists for one gate.
    wasm32-node = mkTarget {
      name = "wasm32-node";
      description = "wasm32 launcher under node";
      cc = "emcc";
      ar = "emar";
      nativeBuildInputs = [ pkgs.emscripten pkgs.nodejs ];
      preBuild = emPreBuild;
      ldflags = [
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=64MB"
        "-sSTACK_SIZE=4MB"
        "-lm"
      ];
      exe = ".js";
      run = "node";
      shell = webShellFor (webShellLdflags ++ [ "-sNODERAWFS=1" ]);
    };
  } // lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
    aarch64 = crossTarget {
      name = "aarch64";
      description = "aarch64 (cross, run under qemu-user)";
      crossPkgs = pkgs.pkgsCross.aarch64-multiplatform-musl;
      qemuBin = "qemu-aarch64";
    };
    riscv64 = crossTarget {
      name = "riscv64";
      description = "riscv64 (cross, run under qemu-user)";
      crossPkgs = pkgs.pkgsCross.riscv64-musl;
      qemuBin = "qemu-riscv64";
    };
  };
in
{
  inherit mkTarget targets baseCFlags;
}
