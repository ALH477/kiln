# SPDX-License-Identifier: MIT
#
# nix/faust.nix — the Faust -> N64 bridge, and the gates that keep it honest.
#
# The feasibility report is a set of numeric constraints that are trivial to
# violate by accident and expensive to discover on hardware. This file is where
# they become build failures instead of prose.
#
# ── Why the gate inspects symbols, not source ──────────────────────────
# The obvious no-libm check is to grep the generated C for `tanh(`, `exp(` etc.
# Measured against real Faust output, that is both over- and under-sensitive:
# a Karplus-Strong voice's `floorf` call is inlined to a single instruction by
# GCC (so the grep fires on something that costs nothing), while Faust's
# internal `fmaxf`/`fminf` are real out-of-line calls that a naive grep of the
# .dsp would never see. So the gate reads the UNDEFINED SYMBOLS of the compiled
# object — the ground truth for what actually gets linked and called.
{ pkgs, toolchain, libdragon }:

let
  lib = pkgs.lib;

  archFile = ../dsp/arch/libdragon_mixer.c;
  offlineArchFile = ../dsp/arch/offline_ref.c;

  # ── Flush-to-zero: why 1 and not the report's 2 ────────────────────────
  # Report §5 prescribes `-double -ftz 2` as the house style for offline
  # renders, and §4 wants flush-to-zero on-console too, because VR4300
  # denormals are handled via exception — expensive enough that libultra's
  # context switch explicitly saves FPU state to avoid the hazard.
  #
  # But `-ftz 2` (mask based) is BROKEN in Faust 2.85.9's C backend, at both
  # precisions and in both block and one-sample mode. The C++ backend emits
  #     ((*reinterpret_cast<int64_t*>(&fTemp0) & 9218868437227405312) ? ... )
  # and the C backend lowers that cast by dropping its parentheses:
  #     ((*((int64_t*(&fTemp0) & 9218868437227405312) ? ... )
  # which does not parse. It is a C-backend cast-lowering bug, not something
  # about our flags.
  #
  # `-ftz 1` is fabs-based rather than mask-based: same semantics, marginally
  # slower, and it compiles. `fabsf` lowers to a single `abs.s` (1 cycle) on
  # the VR4300, so the cost on-console is negligible. Revisit if Faust fixes
  # the C backend.
  ftzMode = "1";

  # libdragon's own codegen flags, mirrored from n64.mk's N64_C_AND_CXX_FLAGS.
  # Kept here rather than shelling out to make, because a Faust voice is a
  # single translation unit and we want to control its flags exactly.
  #
  # Note `-ffast-math`: libdragon enables it, and it is what lets GCC expand
  # fmaxf/fminf and friends inline instead of calling libm.
  n64Flags = [
    "-march=vr4300"
    "-mtune=vr4300"
    "-mabi=o64"
    "-ffast-math"
    "-ftrapping-math"
    "-fno-associative-math"
    "-ffunction-sections"
    "-fdata-sections"
    "-O2"
    "-DN64"
    "-std=gnu17"
  ];

  # Symbols that mean the voice will be far too slow on a 93.75 MHz VR4300
  # with no FPU datapath. Report §4: "Faust's maths.lib maps tanh, exp, sin,
  # pow, etc. to libm, which is both large and slow on the VR4300."
  fatalSymbols = [
    "exp" "expf" "exp2" "exp2f" "expm1" "expm1f"
    "log" "logf" "log2" "log2f" "log10" "log10f" "log1p" "log1pf"
    "pow" "powf"
    "sin" "sinf" "cos" "cosf" "tan" "tanf"
    "asin" "asinf" "acos" "acosf" "atan" "atanf" "atan2" "atan2f"
    "sinh" "sinhf" "cosh" "coshf" "tanh" "tanhf"
    "fmod" "fmodf"
  ];

in
rec {
  # ── mkOfflineRenderer: the host-side, full-quality renderer ───────────
  #
  # Compiled `-double -ftz 2` (the DeMoD house style, and what report §5 calls
  # for when baking). This binary is the shared engine behind both
  # mkBakedInstrument and the golden A/B reference.
  #
  # -ftz 2 is "mask based (fastest)" flush-to-zero on recursive signals. It
  # matters even offline: it is the reference behaviour the on-console build
  # has to approximate by hand, since VR4300 denormals are handled via
  # exception (report §4).
  mkOfflineRenderer =
    { name, src }:
    pkgs.stdenv.mkDerivation {
      pname = "faust-offline-${name}";
      version = "0.1.0";
      src = builtins.path { path = src; name = "${name}.dsp"; };
      dontUnpack = true;

      nativeBuildInputs = [ pkgs.faust pkgs.which ];

      buildPhase = ''
        runHook preBuild
        faust -lang c -double -ftz ${ftzMode} \
              -cn ${name} \
              -a ${offlineArchFile} \
              $src -o render_${name}.c
        $CC -O2 -std=gnu17 -DFAUST_NAME=${name} \
            -I${pkgs.faust}/include \
            render_${name}.c -o render-${name} -lm
        runHook postBuild
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out/bin $out/src
        cp render-${name} $out/bin/
        cp render_${name}.c $out/src/
        runHook postInstall
      '';

      meta.description = "full-quality host renderer for Faust instrument '${name}'";
      meta.mainProgram = "render-${name}";
    };

  # ── mkBakedInstrument: report Stage 1, the recommended 80-90% ─────────
  #
  # Render offline at full quality, then VADPCM-encode with audioconv64 into a
  # .wav64 for libdragon's mixer. Per report §5 this is "where you get the most
  # sonic bang", and Stage 1 is deliberately ordered BEFORE any live synthesis:
  # it de-risks the whole audio path and yields shippable sound immediately.
  #
  # The output is a directory suitable for dropping into a ROM's `filesystem/`
  # so mkdfs picks it up.
  mkBakedInstrument =
    { name
    , src # the .dsp
      # The N64's Audio Interface rate — NOT the M64's fixed 48 kHz HDMI output,
      # which is a reconstruction downstream of whatever the core produces
      # (report §1). Lower rates buy back cycles and RAM.
    , sampleRate ? 32000
    , duration ? 2.0
    , params ? { } # { freq = 220; gain = 0.8; }
    , gate ? null # { param = "gate"; on = 0.0; off = 1.0; }
      # 0 = none, 1 = VADPCM (default), 3 = Opus. VADPCM is a 4-bit ADPCM
      # variant decoded on the RSP's 8-lane SIMD: cheap at runtime, and the
      # right default. Opus is for long streamed cues, not instruments.
    , compress ? 1
    , loop ? false
    , loopOffset ? 0
    , mono ? false
    }:
    let
      renderer = mkOfflineRenderer { inherit name src; };
      paramArgs = lib.concatMap (k: [ "-p" "${k}=${toString params.${k}}" ])
        (builtins.attrNames params);
      gateArgs =
        if gate == null then [ ]
        else [ "-g" "${gate.param}:${toString gate.on}:${toString gate.off}" ];
    in
    pkgs.stdenv.mkDerivation {
      pname = "baked-${name}";
      version = "0.1.0";
      dontUnpack = true;

      nativeBuildInputs = [ renderer libdragon pkgs.python3 ];

      buildPhase = ''
        runHook preBuild

        echo "── render at full quality (-double -ftz ${ftzMode}) ──"
        render-${name} -o ${name}.wav \
          -r ${toString sampleRate} \
          -d ${toString duration} \
          ${lib.escapeShellArgs paramArgs} \
          ${lib.escapeShellArgs gateArgs}

        ls -l ${name}.wav

        echo "── audioconv64 -> wav64 ──"
        mkdir -p filesystem
        audioconv64 -v \
          --wav-compress ${toString compress} \
          ${lib.optionalString mono "--wav-mono"} \
          ${lib.optionalString loop "--wav-loop true --wav-loop-offset ${toString loopOffset}"} \
          -o filesystem ${name}.wav

        runHook postBuild
      '';

      doCheck = true;
      checkPhase = ''
        runHook preCheck
        if [ ! -s filesystem/${name}.wav64 ]; then
          echo "FAIL: audioconv64 produced no ${name}.wav64" >&2
          ls -la filesystem >&2 || true
          exit 1
        fi

        # ── Is there actually a sound in there? ──
        # A baked instrument that renders silence, or that clips, still produces
        # a perfectly valid .wav64 — these are defects you would otherwise only
        # find by ear, after it is already in a ROM. Silence usually means a
        # gate/param label did not match; clipping means gain is too high for
        # this instrument and the render is being clamped.
        python3 - <<'PY'
        import struct, sys, wave
        w = wave.open("${name}.wav")
        n, ch = w.getnframes(), w.getnchannels()
        s = struct.unpack("<%dh" % (n * ch), w.readframes(n))
        peak = max(abs(x) for x in s)
        nz = sum(1 for x in s if x != 0)
        clipped = sum(1 for x in s if abs(x) >= 32767)
        print("  peak: %d/32767  nonzero: %.1f%%  clipped: %d" %
              (peak, 100.0 * nz / len(s), clipped))
        if peak == 0:
            sys.exit("FAIL: render is pure silence — check that the -p/-g "
                     "parameter labels match the .dsp")
        if peak < 328:  # -40 dBFS
            sys.exit("FAIL: render peaks below -40 dBFS; VADPCM will quantise "
                     "this to noise. Raise gain or check the gate envelope.")
        if clipped > len(s) // 1000:
            sys.exit("FAIL: %d samples clipped (>0.1%%). The renderer clamps to "
                     "[-1,1], so this is lost signal, not saturation. Lower "
                     "'gain' for this instrument." % clipped)
        PY
        raw=$(stat -c%s ${name}.wav)
        enc=$(stat -c%s filesystem/${name}.wav64)
        echo "  wav:   $raw bytes"
        echo "  wav64: $enc bytes ($((100 * enc / raw))% of raw)"
        # A .wav64 that is not smaller than the source means compression did
        # not happen — worth catching, since RAM (4 MB, or 8 MB with the M64's
        # built-in Expansion Pak) is the real constraint, not ROM (report §5).
        if [ "${toString compress}" != "0" ] && [ "$enc" -ge "$raw" ]; then
          echo "FAIL: compression requested but output is not smaller" >&2
          exit 1
        fi
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out/filesystem $out/share $out/nix-support
        cp filesystem/${name}.wav64 $out/filesystem/
        # Keep the full-quality render: it is the golden reference for report
        # §Stage 3 A/B validation of any hand-ported RSP kernel.
        cp ${name}.wav $out/share/${name}-reference.wav
        # Export the sample rate so mkN64Rom can cross-check it against the
        # ROM's audio_init rate at build time. A mismatch causes pitch/time
        # drift, not silence — a subtle defect that is hard to catch by ear.
        echo ${toString sampleRate} > $out/nix-support/audio-rate
        runHook postInstall
      '';

      passthru = { inherit renderer sampleRate duration; };

      meta.description = "VADPCM-baked instrument '${name}' for libdragon's mixer";
    };

  # ── mkFaustVoice: a live, on-console voice (report Stages 2/4) ────────
  #
  # Emits an object file to link into a ROM, plus a budget report. The flag set
  # is fixed and not caller-overridable on purpose: `-single` and `-os` are the
  # two decisions the whole feasibility argument rests on.
  mkFaustVoice =
    { name
    , src # the .dsp
    , sampleRate ? 32000
      # Static cycles/sample ceiling. Report §4 puts the realistic CPU budget at
      # ~320-530 cycles/sample; its Stage 2 threshold is "if one voice exceeds
      # ~500 cycles/sample at 32 kHz, drop the output rate, reduce voice count,
      # or move the inner loop to the RSP".
    , budget ? 500
    , faustFlags ? [ ]
    }:
    pkgs.stdenv.mkDerivation {
      pname = "faust-voice-${name}";
      version = "0.1.0";
      src = builtins.path { path = src; name = "${name}.dsp"; };
      dontUnpack = true;

      # faust shells out to `which` during compilation.
      nativeBuildInputs = [ pkgs.faust pkgs.which toolchain ];
      hardeningDisable = [ "all" ];

      # The output is a MIPS relocatable object; nixpkgs' fixup tooling is for
      # host ELFs and has nothing useful to do to it. See rom.nix.
      dontStrip = true;
      dontPatchELF = true;

      buildPhase = ''
        runHook preBuild

        echo "── faust -> C (single precision, one-sample) ──"
        # -single : FAUSTFLOAT=float. Non-negotiable (report §2: DIV.D is 58
        #           cycles vs 29 for DIV.S, against ~2126 cycles/sample total).
        # -os     : one-sample mode; emits frame() and leaves compute() an
        #           empty stub. The architecture file calls frame().
        # -cn     : mangle symbols so several voices can share one ROM.
        #
        # NOT -mem: the report suggests it for deliberate placement of delay-line
        # state, but Faust rejects it for this backend ("-mem0/-mem1 cannot be
        # used with 'c' backend"). It is not needed here anyway — the
        # architecture file holds ONE static instance and never calls
        # new<name>()/delete<name>(), so nothing in the audio path allocates.
        # Those two functions are still emitted (hence calloc/free showing up
        # as undefined symbols) but are unreferenced, and libdragon's
        # -ffunction-sections + --gc-sections drops them at link time.
        # -ftz: flush denormals in recursive signals. Report §4 is explicit that
        # this matters on the VR4300, where denormals trap into an exception
        # handler — precisely the hazard behind libultra saving COP1 state.
        faust -lang c -single -os -ftz ${ftzMode} \
              -cn ${name} \
              -a ${archFile} \
              $src -o ${name}.c

        echo "── cross-compile for the VR4300 ──"
        mips64-elf-gcc ${lib.escapeShellArgs n64Flags} \
          -DFAUST_NAME=${name} \
          -DFAUST_N64_SAMPLE_RATE=${toString sampleRate} \
          -I${pkgs.faust}/include \
          -I${libdragon}/mips64-elf/include \
          -c ${name}.c -o ${name}.o

        runHook postBuild
      '';

      # ── The gates ──────────────────────────────────────────────────
      doCheck = true;
      checkPhase = ''
        runHook preCheck
        fail=0

        echo "── gate: no expensive libm calls ──"
        mips64-elf-nm -u ${name}.o | awk '{print $2}' | sort -u > undefined.txt
        cat undefined.txt
        for sym in ${lib.escapeShellArgs fatalSymbols}; do
          if grep -qx "$sym" undefined.txt; then
            echo "  FATAL: '$sym' is called out-of-line into libm." >&2
            echo "         On the VR4300 this is ruinous. Replace it in the .dsp" >&2
            echo "         with a table lookup: ba.tabulate / ba.tabulateNd, or an" >&2
            echo "         rdtable filled at init. See report §4." >&2
            fail=1
          fi
        done
        [ "$fail" = 0 ] && echo "  clean"

        echo "── gate: no double-precision instructions ──"
        mips64-elf-objdump -d ${name}.o > dis.txt
        # Any .d-suffixed FP op means something was promoted to double behind
        # our back, despite -single. DIV.D alone is 2.7% of a sample budget.
        if grep -qE '\b(add|sub|mul|div|sqrt|neg|abs|mov|c)\.d\b' dis.txt; then
          echo "  FATAL: double-precision instructions emitted:" >&2
          grep -oE '\b[a-z]+\.d\b' dis.txt | sort | uniq -c >&2
          fail=1
        else
          echo "  clean"
        fi

        echo "── static cycle budget (report §2 table) ──"
        # A static estimate: it counts FP instructions in the voice's frame()
        # function weighted by NEC VR4300 latencies, and cannot model cache
        # misses or the ~640 ns RDRAM latency. It is here to catch regressions
        # (a divide creeping into the per-sample path), NOT to predict
        # wall-clock. Trust hardware TICKS profiling for the real number.
        #
        # Scoped to the frame<name> function: init/constructor code contains
        # one-time divisions and table fills that would inflate the count and
        # make the gate fire on voices whose per-sample path is actually clean.
        awk -v fname="frame${name}" '
          # objdump labels look like: 00000000 <framename>:
          $0 ~ "<" fname ">:" { in_frame = 1; next }
          in_frame && /^$/ { in_frame = 0 }
          in_frame && /\tdiv\.s|\tsqrt\.s/  { c += 29; n++ }
          in_frame && /\tmul\.s/            { c += 5;  n++ }
          in_frame && /\tadd\.s|\tsub\.s/   { c += 3;  n++ }
          in_frame && /\tmov\.s|\tneg\.s|\tabs\.s|\tc\.[a-z]+\.s/ { c += 1; n++ }
          END {
            printf "  fp instructions in frame(): %d\n", n
            printf "  weighted cycles (frame only): %d\n", c
          }
        ' dis.txt | tee budget.txt

        echo "  declared budget: ${toString budget} cycles/sample @ ${toString sampleRate} Hz"
        echo "  (static estimate — profile on hardware with TICKS for the real number)"

        # Hard fail if the frame-scoped weighted cycles exceed the budget.
        # The budget is per-sample; frame() processes exactly one sample, so
        # the weighted count IS the per-sample estimate. Report §4: "if one
        # voice exceeds ~500 cycles/sample at 32 kHz, drop the output rate,
        # reduce voice count, or move the inner loop to the RSP".
        weighted=$(awk '/weighted cycles/ {print $NF}' budget.txt)
        if [ -n "$weighted" ] && [ "$weighted" -gt ${toString budget} ]; then
          echo "  FATAL: weighted cycles ($weighted) exceed budget (${toString budget})." >&2
          echo "         Report §4: drop the output rate, reduce voice count, or" >&2
          echo "         move the inner loop to the RSP (Stage 3)." >&2
          fail=1
        fi

        [ "$fail" = 0 ] || { echo "faust voice '${name}' failed its gates" >&2; exit 1; }
        runHook postCheck
      '';

      installPhase = ''
        runHook preInstall
        mkdir -p $out/lib $out/src $out/share
        cp ${name}.o $out/lib/
        cp ${name}.c $out/src/
        cp budget.txt undefined.txt dis.txt $out/share/
        runHook postInstall
      '';

      passthru = { inherit name sampleRate budget; };

      meta.description = "Faust voice '${name}' compiled for the VR4300";
    };
}
