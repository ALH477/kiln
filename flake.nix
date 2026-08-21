# SPDX-License-Identifier: MPL-2.0
{
  description = "Kiln — a Nix build system for Nintendo 64 / ModRetro M64 software";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # libdragon is the SDK (not libultra): modern GCC, the RSP-accelerated
    # mixer, wav64/VADPCM/Opus, XM64, rspq, and the host tools including
    # audioconv64. Pinned as a plain source tree so the rev is explicit.
    #
    # The `preview` branch, not the default `trunk`. This is not optional:
    # Tiny3D's README states it "requires libdragon, specifically the preview
    # branch", and it typedefs its core math types straight off libdragon's
    # fast-math vectors (`typedef fm_vec3_t T3DVec3;`). trunk's fmath.h has only
    # the scalar functions, so on trunk those typedefs silently resolve to
    # nothing and every downstream use fails with errors that look unrelated
    # ("control reaches end of non-void function", "request for member 'm' in
    # something not a structure"). preview is also where third-party N64
    # libraries generally target.
    libdragon = {
      url = "github:DragonMinded/libdragon/preview";
      flake = false;
    };

    # SummerCart64 — provides sc64deployer (ROM upload, debugf stdio,
    # IS-Viewer64, and the AUX PC<->N64 message channel the report's §6
    # networking design uses as its control tunnel).
    summercart64 = {
      url = "github:Polprzewodnikowy/SummerCart64";
      flake = false;
    };

    # StreamDB — the data-management layer. Upstream's C edition cannot run on
    # bare metal (pthreads, flock, fsync, and a 1 KB-per-node trie), so
    # streamdb-embedded/ is a read-only implementation of the same v3 format
    # with a memory strategy suited to 4 MB of RAM.
    streamdb = {
      url = "github:ALH477/DeMoD-StreamDB";
      flake = false;
    };

    # Tiny3D — the 3D half of the engine (RSP-accelerated matrices, models,
    # skinning, particles). libdragon's rdpq provides the 2D half.
    tiny3d = {
      url = "github:HailToDodongo/tiny3d";
      flake = false;
    };

    # N64-UNFLoader — cross-flashcart loader + debugf stdio bridge. ED64 fallback
    # when the connected cart is an FT245R (VID 0403, PID 6001) rather than SC64.
    unfloader-src = {
      url = "github:buu342/N64-UNFLoader";
      flake = false;
    };

    # Claude Code CLI, packaged for Nix. Used only by packages.dev-image (see
    # nix/dev-image.nix) — not part of the N64 build at all.
    claude-code-nix.url = "github:sadjow/claude-code-nix";

    # Builds a NixOS system config into a docker-loadable image. Also only
    # used by packages.dev-image.
    nixos-generators = {
      url = "github:nix-community/nixos-generators";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, libdragon, summercart64, tiny3d, streamdb, unfloader-src, claude-code-nix, nixos-generators }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # The cross toolchain. Owns the whole toolchain strategy; see the file.
        toolchain = import ./nix/toolchain.nix { inherit nixpkgs pkgs system; };

        # libdragon, installed into a store path used as $N64_INST.
        # libdragon's own fast-math library, built for the host. The first
        # brick of the PC target: the engine's fm_vec3_t and 17 fm_* calls are
        # now the REAL ones natively, not a hand-copy. See nix/host-math.nix.
        hostMath = import ./nix/host-math.nix {
          inherit pkgs;
          src = libdragon;
        };

        libdragon-sdk = import ./nix/libdragon.nix {
          inherit pkgs toolchain;
          src = libdragon;
        };

        # Tiny3D, installed with libdragon's layout so the two can be merged.
        tiny3d-sdk = import ./nix/tiny3d.nix {
          inherit pkgs toolchain;
          src = tiny3d;
          libdragon = libdragon-sdk;
        };

        # Data management: StreamDB v3 reader, on-console. Built against
        # libdragon alone (it needs only n64.mk and the DFS backend); keeping
        # it off tiny3d lets it land in n64InstBase, which the engine needs
        # because kiln_asset.c includes <streamdb_embedded.h>.
        streamdb-emb = import ./nix/streamdb.nix {
          inherit pkgs toolchain;
          libdragon = libdragon-sdk;
          src = ./streamdb-embedded;
        };

        # Prefix stage 1: what the engine itself compiles against. Now
        # includes streamdb-emb so kiln_asset.c can find streamdb_embedded.h.
        n64InstBase = import ./nix/n64-inst.nix {
          inherit pkgs;
          libdragon = libdragon-sdk;
          extraLibs = [ tiny3d-sdk streamdb-emb ];
        };

        # The Kiln engine — 3D on Tiny3D, 2D GUI on rdpq, asset layer on
        # streamdb-embedded.
        kiln-engine = import ./nix/engine.nix {
          inherit pkgs toolchain n64InstBase;
          src = ./engine;
        };

        # The asset pipeline: wraps the host tools the merged prefix already
        # ships (gltf_to_t3d, mksprite, mkfont, audioconv64, mkasset) so
        # projects get built assets instead of hand-built geometry, plus
        # mkStreamdb to pack any of them into a single .streamdb container.
        assetLib = import ./nix/assets.nix {
          inherit pkgs;
          n64Inst = n64InstBase;
          streamdbSrc = streamdb;
        };

        # Demo assets, converted from the hand-authored sources in assets/.
        # Exercised by assets-demo below and by the determinism check.
        demoModel = assetLib.mkModel {
          name = "cube";
          src = ./assets/cube.gltf;
          # Hand-authored glTF, not exported by Blender's fast64 add-on, so it
          # carries none of the custom material properties gltf_to_t3d expects
          # (it aborts with "Material has no fast64 data!" otherwise). Real
          # models come out of the fast64 pipeline and won't need this.
          ignoreMaterials = true;
          # The source cube is a 1x1x1 Blender unit cube; the default
          # baseScale of 64 turns that into a 64-unit cube that overfills the
          # frustum at the demo's camera distance. 28 matches examples/engine's
          # hand-built cube (half-extent 14) for a comparable picture.
          baseScale = 28;
        };
        demoSprite = assetLib.mkSprite {
          name = "logo";
          src = ./assets/logo.png;
          format = "RGBA32";
        };
        demoSound = assetLib.mkSound {
          name = "blip";
          src = ./assets/blip.wav;
        };

        # Footstep sample for the Phase 3 surface/shader demo. Same mkSound
        # path as blip; ships at rom:/sfx/step.wav64 so clip-demo can load it
        # alongside blip and pick one per surface.
        stepSound = assetLib.mkSound {
          name = "step";
          src = ./assets/step.wav;
        };

        # A 2-bone rigged/skinned + animated test model (tools/gen_skel_gltf.py)
        # for camera-skel-demo. Same ignoreMaterials reasoning as demoModel —
        # hand-authored, not a fast64 export. baseScale 32 (half the default
        # 64) keeps the ~1-2 Blender-unit rig in the same size range as
        # examples/actors-demo's hand-built cubes (half-extent 8-14).
        skelModel = assetLib.mkModel {
          name = "rig";
          src = ./assets/skel_test.gltf;
          ignoreMaterials = true;
          baseScale = 32;
        };

        # ── StreamDB-destined demo assets ──────────────────────────────
        # Three assets packed into a single .streamdb, exercising every
        # kiln_asset accessor: kiln_asset_model (cube.t3dm), kiln_asset_sprite
        # (logo.sprite), and kiln_asset_load on a raw level-layout blob.
        #
        # IMPORTANT: compress = 0 across the board. kiln_asset_load returns
        # the StreamDB payload bytes verbatim — there is no asset_load in the
        # path to decompress them, so a mkasset-compressed .t3dm/.sprite
        # would arrive at t3d_model_load_buf / sprite_load_buf still
        # compressed and fail to parse. Loose-DFS assets-demo uses
        # compress = 2/1 because t3d_model_load / sprite_load call asset_load
        # internally; the StreamDB path bypasses asset_load, so the
        # compression step must be skipped at build time too. The .streamdb
        # container itself is the single compressed artefact if you want
        # compression — apply it there, not at the per-asset level.
        sdModel = assetLib.mkModel {
          name = "cube";
          src = ./assets/cube.gltf;
          ignoreMaterials = true;
          baseScale = 28;
          compress = 0;
        };
        sdSprite = assetLib.mkSprite {
          name = "logo";
          src = ./assets/logo.png;
          format = "RGBA32";
          compress = 0;
        };
        # A level-layout blob: 4-byte magic 'KLNL' + u32 spawn-count + N vec3
        # spawn points. Generated deterministically (no binary checked in).
        introLevel = pkgs.runCommandLocal "intro-level.bin" {
          nativeBuildInputs = [ pkgs.python3 ];
        } ''
          python3 -c '
          import struct, sys
          spawns = [(0,0,0),(10,0,10),(-10,0,10),(0,5,0)]
          with open(sys.argv[1], "wb") as f:
              f.write(b"KLNL")
              f.write(struct.pack("<I", len(spawns)))
              for x,y,z in spawns:
                  f.write(struct.pack("<fff", x, y, z))
          ' $out
        '';
        sdLevel = assetLib.mkRawAsset {
          name = "intro";
          src = introLevel;
          dest = "levels";
          compress = 0;
          extension = "bin";
        };
        # Loose Quake .map shipped to rom:/maps/ for kiln_map at runtime.
        quakeMap = assetLib.mkRawAsset {
          # The NAME IS THE FILENAME. It was "quake-test-map", shipping
          # maps/quake-test-map.map, while examples/map-demo/main.c:89 opens
          # rom:/maps/quake_test.map — so the map never loaded, exactly as
          # CLAUDE.md's "an asset builder's `name` is the FILENAME" entry
          # describes. It went unnoticed for as long as it did because the ROM
          # had no filesystem at all (see the Makefile), which failed first.
          name = "quake_test";
          src = ./assets/quake_test.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # Two-room + enemies test map for examples/oot-demo. Same raw-asset
        # path as quakeMap so kiln_map reads it via rom:/maps/oot_test.map —
        # which this entry CLAIMED and did not do: the name was "oot-test-map",
        # so it shipped maps/oot-test-map.map while
        # examples/oot-demo/main.c:222 asked for oot_test.map. A comment
        # asserting the path is not the same as the name producing it.
        ootMap = assetLib.mkRawAsset {
          name = "oot_test";
          src = ./assets/oot_test.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        demoStreamdb = assetLib.mkStreamdb {
          name = "assets";
          entries = [
            { key = "models/cube.t3dm";    asset = sdModel; }
            { key = "sprites/logo.sprite"; asset = sdSprite; }
            { key = "levels/intro.bin";    asset = sdLevel; }
          ];
        };

        # Geometry authoring. Owns the whole Blender strategy; see the file for
        # why Fast64 is deliberately not vendored.
        blenderLib = import ./nix/blender.nix {
          inherit pkgs;
          n64Inst = n64InstBase;
        };

        # Procedural textures. One derivation feeds two consumers: the PNGs
        # gltf_to_t3d decodes at conversion time to learn UV pixel dimensions,
        # and the .sprites the ROM ships. See nix/assets.nix.
        textures = assetLib.mkTextures { name = "kiln"; };

        # ── Test geometry ─────────────────────────────────────────────
        # Each model isolates one class of renderer bug; tools/blender/models.py
        # documents which. Untextured models go through the `shade` preset,
        # which reproduces libdragon's RDPQ_COMBINER_SHADE exactly — the same
        # combiner kiln_scene_begin() already sets, so they compose with
        # hand-built geometry in the same pass.
        mkTestModel = model: extra: blenderLib.mkBlenderModel ({
          name = model;
          script = "models.py";
        } // extra);

        testModels =
          # Vertex-coloured. No texture means no TMEM and no tile setup, and
          # the `shade` preset leaves the scene's own RDP state alone.
          builtins.listToAttrs (map
            (n: { name = n; value = mkTestModel n { }; })
            [ "axes" "cube" "sphere" "cylinder" "cone" "torus" "stress" ])
          // {
            checker = mkTestModel "checker" {
              inherit textures;
              materials = [
                "CheckerWrapMat=tex0_shade,tex=textures/checker.i8.png,size=32"
                # Same texture, mirrored repeat — the difference between the two
                # quads is the whole point of the model.
                "CheckerMirrorMat=tex0_shade,tex=textures/checker.i8.png,size=32,mirror=true"
              ];
            };
            uvsphere = mkTestModel "uvsphere" {
              inherit textures;
              materials = [
                "GridMat=tex0_shade,tex=textures/grid.rgba16.png,size=32"
              ];
            };
          };

        # The rigged/animated reference — the only thing here exercising
        # Tiny3D's skinning + animation (t3dskeleton.h/t3danim.h) through the
        # Blender-authoring path (kiln_skel.h's runtime side is exercised
        # separately by examples/camera-skel-demo's hand-authored rig).
        # tools/blender/goblin.py documents why every part is rigidly bound
        # to exactly one bone.
        goblinModel = blenderLib.mkBlenderModel {
          name = "goblin";
          script = "goblin.py";
          animated = true;
        };

        # Ganja Goblin's four playable characters, from the same script and
        # the same skeleton — see tools/blender/goblin.py on why the rig is
        # byte-identical across all four. That is what lets one set of
        # animations (Idle/Walk/Wave/Taunt plus the five Ride* actions) play
        # on whichever character a player picked, instead of four copies of
        # the animation data in the ROM.
        #
        # They are separate derivations rather than one model with four
        # palettes because the geometry genuinely differs — Moss is a wider
        # mass with clumps growing on him, Sparky is thinner with goggles —
        # and a runtime palette swap cannot do that.
        goblinCast = pkgs.lib.genAttrs
          [ "dank" "sparky" "moss" "glimmer" ]
          (character: blenderLib.mkBlenderModel {
            name = character;
            script = "goblin.py";
            model = character;
            animated = true;
          });

        # The hero prop: not a test shape, but a piece of content authored the
        # way a game's content is — one silhouette from six interpenetrating
        # parts, shaded entirely by COLOR_0 through the same `shade` combiner
        # kiln_scene_begin() already sets, so it composes with hand-built
        # geometry in one pass and needs no TMEM. tools/blender/interceptor.py
        # documents the orientation and the budget.
        interceptorModel = blenderLib.mkBlenderModel {
          name = "interceptor";
          script = "interceptor.py";
        };

        # The goblins' rides. Same authoring path as interceptorModel — one
        # silhouette from interpenetrating solids, COLOR_0 only, no TMEM —
        # but built with kilnlib's sweep()/rotated(), which exist because a
        # vehicle is mostly swept tube (exhaust, roll bars, fenders, forks)
        # and hand-rolling that frame per part is where inside-out geometry
        # comes from. tools/blender/test_vehicles.py checks every part's
        # signed volume on the host before Blender is ever started.
        #
        # `--accent RRGGBB` retints the bodywork only, so four karts that
        # match gg_player_tint's four seat colours are four derivations over
        # one script and no runtime support — cheaper than four textures.
        # Only the default green is built here; add a variant when the game
        # actually places per-player vehicles on the board.
        gokartModel = blenderLib.mkBlenderModel {
          name = "gokart";
          script = "vehicles.py";
        };
        bikeModel = blenderLib.mkBlenderModel {
          name = "bike";
          script = "vehicles.py";
        };

        # Cinematic-demo extras: a service droid (rigged+animated, two bones
        # driving a wave animation), an approaching alien (taller, six-legged
        # silhouette, head-bob animation), and the hangar.map that lays out
        # the room. Same mkBlenderModel path as goblinModel / interceptorModel.
        droidModel = blenderLib.mkBlenderModel {
          name = "droid";
          script = "droid.py";
          animated = true;
        };
        alienModel = blenderLib.mkBlenderModel {
          name = "alien";
          script = "alien.py";
          animated = true;
        };
        # Hand-authored Quake .map for the cinematic-demo's hangar room.
        # Same mkRawAsset path as quakeMap / ootMap so kiln_map reads it via
        # rom:/maps/hangar.map at runtime. compress=0 because kiln_map_load
        # is the consumer — there is no asset_load in the path, so a
        # compressed .map would arrive still-compressed and fail to parse.
        hangarMap = assetLib.mkRawAsset {
          name = "hangar-map";
          src = ./assets/hangar.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };

        # Verifies mkQuakeMapModel through the hermetic pipeline end to end:
        # a single 6-plane cube brush, the same content
        # tools/blender-mcp/server.py's own inspect/import tools were checked
        # against before this Nix wiring was written.
        # FPS level: a larger Quake .map with multiple enemies, loaded at
        # runtime via kiln_map (same raw-asset path as ootMap / hangarMap).
        fpsMap = assetLib.mkRawAsset {
          name = "fps-level-map";
          src = ./assets/fps_level.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # Per-room maps for the multi-room streaming FPS.
        fpsRoom0 = assetLib.mkRawAsset {
          name = "fps_room0";
          src = ./assets/fps_room0.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        fpsRoom1 = assetLib.mkRawAsset {
          name = "fps_room1";
          src = ./assets/fps_room1.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        fpsRoom2 = assetLib.mkRawAsset {
          name = "fps_room2";
          src = ./assets/fps_room2.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # SFX for the FPS: gunfire, wall impact, enemy hit, pickup, metal impact.
        gunshotSfx = assetLib.mkSound { name = "gunshot"; src = ./assets/gunshot.wav; };
        impactSfx  = assetLib.mkSound { name = "impact";  src = ./assets/impact.wav; };
        enemyHitSfx = assetLib.mkSound { name = "enemy-hit"; src = ./assets/enemy_hit.wav; };
        pickupSfx  = assetLib.mkSound { name = "pickup";  src = ./assets/pickup.wav; };
        impactMetalSfx = assetLib.mkSound { name = "impact-metal"; src = ./assets/impact_metal.wav; };
        doorOpenSfx = assetLib.mkSound { name = "door-open"; src = ./assets/door_open.wav; };
        doorLockedSfx = assetLib.mkSound { name = "door-locked"; src = ./assets/door_locked.wav; };
        chestOpenSfx = assetLib.mkSound { name = "chest-open"; src = ./assets/chest_open.wav; };
        explosionSfx = assetLib.mkSound { name = "explosion"; src = ./assets/explosion.wav; };
        rocketFireSfx = assetLib.mkSound { name = "rocket-fire"; src = ./assets/rocket_fire.wav; };
        plasmaFireSfx = assetLib.mkSound { name = "plasma-fire"; src = ./assets/plasma_fire.wav; };
        shotgunFireSfx = assetLib.mkSound { name = "shotgun-fire"; src = ./assets/shotgun_fire.wav; };
        npcTalkSfx = assetLib.mkSound { name = "npc-talk"; src = ./assets/npc_talk.wav; };

        quakeTestModel = blenderLib.mkQuakeMapModel {
          name = "quake-test";
          src = ./assets/quake_test.map;
        };

        # Prefix stage 2: the single prefix every ROM build sees as $N64_INST.
        # streamdb-emb is already in n64InstBase; listed here only for clarity
        # (symlinkJoin dedupes by store path, so no double-mount).
        n64Inst = import ./nix/n64-inst.nix {
          inherit pkgs;
          libdragon = libdragon-sdk;
          extraLibs = [ tiny3d-sdk kiln-engine streamdb-emb ];
        };

        # The ROM builder. Projects bring a Makefile; this supplies the
        # hermetic environment. See nix/rom.nix for why.
        mkN64Rom = import ./nix/rom.nix {
          inherit pkgs toolchain n64Inst;
        };

        hello = mkN64Rom {
          name = "hello";
          src = ./examples/hello;
          romTitle = "Kiln Hello";
        };

        # The Faust bridge and the gates that enforce the report's constraints.
        faust = import ./nix/faust.nix {
          inherit pkgs toolchain;
          libdragon = n64Inst;
        };

        # A live single-precision voice, used as the worked example and as the
        # regression test for the no-libm / no-double gates.
        ks-voice = faust.mkFaustVoice {
          name = "ksvoice";
          src = ./dsp/ks.dsp;
          sampleRate = 32000;
        };

        # The same instrument taken down the OTHER path — rendered at full
        # quality on the host and VADPCM-encoded. Report §5's hybrid
        # recommendation is to bake most of the audio this way and reserve live
        # synthesis for what must be parametric, so both paths are first-class.
        ks-baked = faust.mkBakedInstrument {
          name = "ksvoice";
          src = ./dsp/ks.dsp;
          sampleRate = 32000;
          duration = 2.0;
          # gain 0.1 is not arbitrary: pm.ks runs hot, and anything above ~0.1
          # clips against the renderer's [-1,1] clamp. The clipping gate in
          # mkBakedInstrument found this — at the obvious-looking 0.8, 15% of
          # samples were clamped. Peak here is -2.8 dBFS.
          params = { freq = 220; gain = 0.1; };
          gate = { param = "gate"; on = 0.0; off = 0.02; };
        };

        # Report Stage 1 proved end to end: bake -> DFS -> RSP mixer -> ROM.
        audio = mkN64Rom {
          name = "audio";
          src = ./examples/audio;
          romTitle = "Kiln Audio";
          assets = [ ks-baked ];
          audioRate = 32000;
        };

        # Live Faust voice + baked instrument A/B comparison (report Stage 2).
        # Links the ks-voice MIPS object into the ROM and renders it
        # sample-by-sample on the VR4300, mixed with the RSP mixer output.
        live-voice = mkN64Rom {
          name = "live-voice";
          src = ./examples/live-voice;
          romTitle = "Kiln Live Voice";
          assets = [ ks-baked ];
          audioRate = 32000;
          makeFlags = [ "FAUST_VOICE=${ks-voice}/lib/ksvoice.o" ];
        };

        # 3D + 2D GUI worked example.
        engine-demo = mkN64Rom {
          name = "engine";
          src = ./examples/engine;
          romTitle = "Kiln Engine";
        };

        # Open-world streaming demo: scratch allocator, refcounted cache,
        # tile residency manager, LOD selector, two-pass renderer.
        openworld-demo = mkN64Rom {
          name = "openworld-demo";
          src = ./examples/openworld-demo;
          romTitle = "Kiln Open World";
        };

        # XM64 tracker music playback example. mkMusic converts the .xm
        # via audioconv64; the ROM plays it through libdragon's XM64 player.
        test-music = assetLib.mkMusic {
          name = "test";
          src = ./examples/music/test.xm;
        };

        # Cinematic music bed — a 15s dark-sci-fi loop synthesised in
        # examples/music/synth_loop.py (no MIDI, no soundfont, no .xm — just
        # sine + saw + noise at 32 kHz). audioconv64 turns it into VADPCM
        # .wav64 at ~250 KB; libdragon's wav64 player loops it natively. We
        # bypass the .xm/xm_tick path entirely so the BPM-0 divide-by-zero
        # family of bugs (and xm_tick's libm+float ops) is off the table.
        cine-music = assetLib.mkSound {
          name = "cine_loop";
          src = ./examples/music/cine_loop.wav;
          loop = true;
        };

        music-demo = mkN64Rom {
          name = "music";
          src = ./examples/music;
          romTitle = "Kiln Music";
          assets = [ test-music ];
          audioRate = 32000;
        };

        sc64deployer = import ./nix/tools/sc64deployer.nix {
          inherit pkgs;
          src = summercart64;
        };

        unfloader = import ./nix/tools/unfloader.nix {
          inherit pkgs;
          src = unfloader-src;
        };

        # Phase A verification: a ROM that loads one of each converted asset
        # kind — proves gltf_to_t3d, mksprite and audioconv64 actually run,
        # not just that the Nix glue around them evaluates.
        assets-demo = mkN64Rom {
          name = "assets-demo";
          src = ./examples/assets-demo;
          romTitle = "Kiln Assets";
          assets = [ demoModel demoSprite demoSound ];
          audioRate = 32000;
        };

        # Phase B verification: the actor system (engine/src/kiln/kiln_actor.*)
        # with one profile per category that matters here — spawn, handle-based
        # despawn, an actor despawning itself mid-update, and the fixed
        # category draw order all exercised in one ROM.
        actors-demo = mkN64Rom {
          name = "actors-demo";
          src = ./examples/actors-demo;
          romTitle = "Kiln Actors";
        };

        # Scene/room streaming — a 2×2 grid of rooms, camera starts in room A.
        # The kiln_room module loads the room under the camera and its
        # neighbours; HUD reports current room + loaded count.
        rooms-demo = mkN64Rom {
          name = "rooms-demo";
          src = ./examples/rooms-demo;
          romTitle = "Kiln Rooms";
        };

        # Phase B completion: kiln_camera (OoT-style spring-arm follow) +
        # kiln_skel (skeletal animation, idle/swing blend) + kiln_audio
        # (footstep SFX on distance travelled) together in one ROM, driving
        # the kiln_actor player already exercised by actors-demo.
        camera-skel-demo = mkN64Rom {
          name = "camera-skel-demo";
          src = ./examples/camera-skel-demo;
          romTitle = "Kiln Camera Skel";
          assets = [ skelModel demoSound ];
          audioRate = 32000;
        };

        # Phase C verification: same three asset kinds as assets-demo, but
        # loaded from a single StreamDB container mounted at boot via
        # kiln_asset_open. Exercises kiln_asset_model (the patched
        # t3d_model_load_buf path), kiln_asset_sprite (sprite_load_buf), and
        # kiln_asset_load on a raw level blob, plus kiln_asset_count and
        # kiln_asset_find_suffix.
        streamdb-demo = mkN64Rom {
          name = "streamdb-demo";
          src = ./examples/streamdb-demo;
          romTitle = "Kiln StreamDB";
          assets = [ demoStreamdb ];
        };

        # Phase C step 1: kiln_input (deadzoned joypad wrapper with button
        # edges) + kiln_clip (swept-AABB-vs-brushes collision with iterative
        # SlideMove). One player box pushed around a 5-brush room; the box
        # slides along walls, HUD reports the last trace's fraction / normal /
        # surface. No assets, no actors — the proof stays focused on the
        # collision primitive.
        clip-demo = mkN64Rom {
          name = "clip-demo";
          src = ./examples/clip-demo;
          romTitle = "Kiln Clip";
          assets = [ demoSound stepSound ];
        };

        # Phase E: kiln_room brush auto-install + kiln_clip broadphase toggle
        # + kiln_physics HL2-style rigid bodies. One room (floor + 4 walls,
        # brushes auto-installed via kiln_room); 6 dynamic crate bodies fall,
        # stack, rest, sleep; A punts the nearest crate in a forward cone
        # (gravity-gun feel); D-pad toggles PHYS ON/OFF and BP ON/OFF; HUD
        # shows the last trace's brush count so the broadphase win is visible.
        physics-demo = mkN64Rom {
          name = "physics-demo";
          src = ./examples/physics-demo;
          romTitle = "Kiln Physics";
        };

        # Phase C step 2: kiln_dict + kiln_map. Loads assets/quake_test.map,
        # parses it into brushes + face quads, and spawns the player at the
        # info_player_start entity by reading "origin" from the KilnDict.
        map-demo = mkN64Rom {
          name = "map-demo";
          src = ./examples/map-demo;
          romTitle = "Kiln Map";
          assets = [ quakeMap ];
        };

        # Phase 4: kiln_event. A switch actor posts DOOR_OPEN with a 500 ms
        # delay; the door actor's event callback rotates it open. HUD shows
        # the queued-event count so the 500 ms gap is visible.
        event-demo = mkN64Rom {
          name = "event-demo";
          src = ./examples/event-demo;
          romTitle = "Kiln Event";
        };

        # Phase 6: the OoT + id Tech 4 integration proof. A player actor
        # (kiln_player locomotion) walks an oot_test.map room, slides via
        # kiln_clip, Z-targets enemies (kiln_target + camera TARGETING mode),
        # and emits footstep SFX through kiln_event + kiln_sound shaders.
        oot-demo = mkN64Rom {
          name = "oot-demo";
          src = ./examples/oot-demo;
          romTitle = "Kiln OoT";
          assets = [ ootMap stepSound ];
        };

        # Same integration proof, but with the retro console + profiler wired in.
        # This is the debug build of oot-demo; keep the vanilla one lean.
        oot-demo-debug = mkN64Rom {
          name = "oot-demo-debug";
          src = ./examples/oot-demo;
          romTitle = "Kiln OoT Debug";
          assets = [ ootMap stepSound ];
          debugConsole = true;
        };

        # The on-screen retro debug console. Built with `debugConsole = true`
        # so KILN_DEBUG=1 reaches the example's main.c (gating the
        # kiln_console_* calls). The console module itself is always in
        # libkiln.a; the flag only controls whether the example wires it up.
        # Toggle in-rom by holding Start and pressing C-Up → C-Left →
        # C-Down → C-Right (counter-clockwise around the C cluster).
        debug-demo = mkN64Rom {
          name = "debug-demo";
          src = ./examples/debug-demo;
          romTitle = "Kiln Debug";
          debugConsole = true;
        };

        # The single-screen showcase: title + 3-mode flight + engine streaks +
        # credit HUD. Loads the hand-authored Interceptor starfighter through
        # the same mkBlenderModel path tools/blender/interceptor.py documents.
        interceptor-demo = mkN64Rom {
          name = "interceptor-demo";
          src = ./examples/interceptor-demo;
          romTitle = "Kiln Interceptor";
          assets = [ interceptorModel demoSound test-music ];
          audioRate = 32000;
        };

        # texanim-demo: exercises kiln_texanim (UV scroll, flipbook, palette,
        # offscreen) and kiln_vanim (procedural deform, morph blending, RSP
        # vertex FX). All geometry is hand-built — no asset pipeline needed.
        texanim-demo = mkN64Rom {
          name = "texanim-demo";
          src = ./examples/texanim-demo;
          romTitle = "Kiln TexAnim";
        };

        # Cinematic-demo: a 60-second single-shot scene of the Interceptor in
        # its hangar with the goblin captain walking the perimeter, droids
        # servicing the ship, and aliens approaching from the back. Exercises
        # every engine subsystem in one ROM — input, player, clip, target,
        # surface, sound, event, dict, map, room, camera (CUTSCENE mode), skel
        # (goblin walk-cycle), audio (music + SFX). Assets:
        #   interceptorModel / goblinModel / droidModel / alienModel — the cast
        #   hangarMap           — the Quake-format .map room
        #   demoSound / stepSound — SFX (engine whoosh, footsteps)
        #   cine-music          — VADPCM .wav64 loop (15s dark-sci-fi bed)
        cinematic-demo = mkN64Rom {
          name = "cinematic-demo";
          src = ./examples/cinematic-demo;
          romTitle = "Kiln Cinematic";
          assets = [
            interceptorModel goblinModel droidModel alienModel
            hangarMap demoSound stepSound cine-music
          ];
          audioRate = 32000;
        };

        # A minimal playable first-person shooter. First-person camera
        # (kiln_fpscam), hitscan weapon (kiln_weapon), enemy actors that chase
        # the player, HUD with crosshair + health + ammo. The FPS level is a
        # Quake .map loaded at runtime via kiln_map.
        fps = mkN64Rom {
          name = "fps";
          src = ./examples/fps;
          romTitle = "Kiln FPS";
          assets = [ fpsRoom0 fpsRoom1 fpsRoom2 gunshotSfx impactSfx enemyHitSfx pickupSfx impactMetalSfx doorOpenSfx doorLockedSfx chestOpenSfx explosionSfx rocketFireSfx plasmaFireSfx shotgunFireSfx npcTalkSfx ];
          audioRate = 32000;
        };

        # ── Bass synth ────────────────────────────────────────────────────
        # 4-controller collaborative bass ROM. 12 dual-layer wavetables
        # (4 engines x {body_bright, body_dark, sub}) generated by
        # tools/gen_bass_wav.py, baked as looping VADPCM wav64s and
        # pitched live by the RSP mixer. Each held note consumes 2 mixer
        # channels (body + sub) → 6-voice polyphony across 12 channels.
        bassWav = engine: layer: assetLib.mkSound {
          name = "bass_${engine}_${layer}";
          src = ./assets/bass_wav/${engine}_${layer}.wav;
          loop = true;
          mono = true;
        };
        bassWavFlat = let
          layers = [ "body_bright" "body_dark" "sub" ];
          engines = [ "heavy" "sub" "growl" "industrial" ];
        in pkgs.lib.concatMap (e: map (l: bassWav e l) layers) engines;

        bass-synth = mkN64Rom {
          name = "bass-synth";
          src = ./examples/bass-synth;
          romTitle = "Kiln Bass Synth";
          assets = bassWavFlat;
          audioRate = 32000;
        };

        # ── Ganja Goblin ─────────────────────────────────────────────────
        # A standalone top-level game (not an examples/ entry). Standalone
        # because it's a real product target, not a worked example: its own
        # package namespace, its own game/src/, its own game/assets/, sized
        # for a releaseable ROM rather than a single-file demo. Phase 0 wires
        # up the skeleton; engine primitives (RNG, dice, board, turn) land
        # in Phase 1, the game-side board loop in Phase 2, the 4 goblins in
        # Phase 3, menus/HUD in Phase 4, items/status in Phase 5, audio in
        # Phase 6, particle VFX + polish in Phase 7. Mini-games are deferred
        # (Phase 8, future work).
        #
        # See /home/asher/.claude/plans/ganja-goblin-is-a-buzzing-rabbit.md
        # for the full roadmap. No assets yet — they arrive in Phase 3+
        # (goblin models) and Phase 6 (audio).
        ganja-goblin = mkN64Rom {
          name = "ganja-goblin";
          src = ./game;
          romTitle = "Ganja Goblin";
          saveType = "eeprom4k"; # match-progress + per-goblin unlock flags
        };

        # ── Forge ────────────────────────────────────────────────────────
        # A standalone tool ROM: a voxel level/cinematic editor that runs on the
        # console, so a level is judged where it will be played rather than two
        # tool hops away. See the plan in
        # /home/asher/.claude/plans/minecraft-like-game-that-fluttering-minsky.md
        #
        # `forge-selftest` comes FIRST and is deliberately its own ROM. Forge's
        # whole iteration loop rests on one assumption — that a ROM can write
        # files to the SD card of an ED64 Plus, which is a clone board libcart
        # names but nobody here has proved. This probe answers that in one boot,
        # exercising kiln_store's real write path rather than a copy of it.
        #
        # saveType is DELIBERATELY not eeprom/sram here: the probe reports
        # whether the SD path works, and a declared save type would have the
        # EverDrive OS offering to flush a save that the probe is not using.
        # The SRAM fallback gets its own variant below when it is needed.
        forge-selftest = mkN64Rom {
          name = "forge-selftest";
          src = ./Forge/selftest;
          romTitle = "Kiln Forge Probe";
        };

        # The editor itself. No `assets` and no saveType: the level lives on the
        # SD card, not in the ROM and not in a save chip, which is exactly the
        # property that makes editing cost no rebuild. A DFS-seeded variant for
        # emulator inspection lands with the mode work that needs it.
        forge = mkN64Rom {
          name = "forge";
          src = ./Forge;
          romTitle = "Kiln Forge";
        };

        # Forge with a level baked into the ROM. Its only purpose is to make the
        # LOAD path verifiable without a cart: `./dev shot forge-dfs` boots it,
        # kiln_store falls through SD and the save chip to read-only `rom:/`, and
        # the level either appears or the HUD says which step refused.
        forge-dfs = mkN64Rom {
          name = "forge-dfs";
          src = ./Forge;
          romTitle = "Kiln Forge DFS";
          assets = [ forgeSeedLevel ];
        };

        # `nix build .#forge-<mode>` boots straight into one mode over the baked
        # level. `./dev shot` has no input path at all by design and `./dev
        # drive`'s uinput chain is fragile, so a mode reached only by a chord is
        # a mode that can only be verified by hand — which for WALK (the one
        # whose entire purpose is standing in the level) and CAM (whose whole
        # output is a curve you have to SEE) is most of the value. Same idiom and
        # the same reasoning as the pm-jump ROMs.
        # No `assets`, deliberately: with nothing to load these fall through to
        # forge_io_seed's demo content, which is a room WITH a spawn and a
        # three-key shot. The baked level is geometry only — frg.py imports a
        # `.map`, and a `.map` has no camera path — so a CAM capture over it
        # correctly reported `ERR nokeys` and showed an empty timeline.
        #
        # The alternative was teaching frg.py to invent a camera path on import,
        # which would put content nobody authored into every level anyone
        # imported. So the demo content has exactly ONE author (forge_io_seed),
        # and `.#forge-dfs` still covers the load path it is there to cover.
        forgeModes = [ "WALK" "PAINT" "ENT" "LIGHT" "CAM" ];
        forgeModeRoms = pkgs.lib.listToAttrs (map
          (m: pkgs.lib.nameValuePair "forge-${pkgs.lib.toLower m}" (mkN64Rom {
            name = "forge-${pkgs.lib.toLower m}";
            src = ./Forge;
            romTitle = "Kiln Forge ${m}";
            makeFlags = [ "FORGE_MODE=${m}" ];
          }))
          forgeModes);

        # The same probe with a save chip declared, which is the ONLY way to
        # exercise kiln_store's SRAM fallback: `sram_detect()` round-trips a word
        # through the cart, so with no save type in the ROM header there is
        # nothing there to detect and the fallback correctly refuses itself.
        #
        # Two ROMs rather than one flag because this is a gate on a fallback and
        # a gate should be verified to fire in BOTH directions — the plain build
        # must report "no writable backend" under an emulator, this one must
        # round-trip 8 KB through SRAM. Testing only the arm that passes is how
        # you ship a fallback that was never once exercised.
        forge-selftest-sram = mkN64Rom {
          name = "forge-selftest-sram";
          src = ./Forge/selftest;
          romTitle = "Kiln Forge Probe SRAM";
          saveType = "sram256k";
        };

        # Phase 1 verification: kiln_rng + kiln_dice + kiln_board + kiln_turn
        # end-to-end. 4 tokens, 5 rounds, a 10-node branching path, an
        # auto-advancing state machine. No assets — the proof is the
        # topology and the turn transitions, drawn as a 2D HUD schematic.
        board-demo = mkN64Rom {
          name = "board-demo";
          src = ./examples/board-demo;
          romTitle = "Kiln Board";
        };

        # ── PetaByte Madness ─────────────────────────────────────────────
        # The second standalone game target, same shape as ganja-goblin
        # above: its own top-level directory, its own package namespace, its
        # own assets. A first-person horror game in an underwater lab, built
        # around one mechanic — the scarlet veil, a filter the player raises
        # to see the demons, which raises their ability to see the player
        # too. PetaByte-Madness/docs/VEIL_DESIGN.md is the spec for that
        # mechanic in the same way the compass report is the spec for the
        # build system; read it before changing PetaByte-Madness/src/pm_veil.*.
        #
        # Unlike ganja-goblin, this one arrived with its art: an asset drop
        # of rigged demons, a work submarine, guards, and the lab itself
        # lives in PetaByte-Madness/archives/. PetaByte-Madness/README.md
        # says what came from where and what is not yet on a hermetic path.
        #
        # The four demon models are the only glTF in the drop and so the
        # only meshes mkModel can eat today; --ignore-materials is required
        # because they carry no fast64 material block (see CLAUDE.md's
        # "gltf_to_t3d aborts on a glTF material with no fast64 data").
        # The machine centaur — Dr. Horner after the MRI, and the player
        # character. Not on the mkModel path the demons use, because the
        # demons arrive as .glb NODE animations on an unskinned hierarchy and
        # gltf_to_t3d drops every one of those channels ("Channel target not
        # found"). The centaur is one bone per limb across 23 bones, which is
        # one bone per vertex — exactly the rigid binding the importer wants —
        # so it is rebuilt as a real armature and the 13 animations survive.
        #
        # Two conversions stand between the original F3DEX2 rig and this, and
        # each carries its own numerical self-test rather than an argument:
        #   PetaByte-Madness/tools/mc_rig_export.py  ->  .json  (--verify)
        #   tools/blender/centaur.py                 ->  .gltf  (--selftest)
        # See both files' headers; the second one is why the coordinate
        # change is (x,-z,y) and not the reflection (x,z,y).
        # The ambience bed: a 55 Hz pressure drone with the design's
        # gameplay pulse baked into it (docs/VEIL_DESIGN.md §7). Baked
        # rather than live for the reason report Stage 1 gives — it never
        # has to respond to anything, so every cycle it would cost on the
        # VR4300 is a cycle the demons get to keep.
        #
        # 8 seconds and looping: long enough that the detune beats between
        # the three partials do not audibly repeat, short enough to sit in
        # RAM without a streamed read. mkBakedInstrument's silence,
        # over-quiet and clipping gates all apply.
        pmDrone = faust.mkBakedInstrument {
          name = "pmdrone";
          src = ./PetaByte-Madness/dsp/pm_drone.dsp;
          sampleRate = 32000;
          duration = 8.0;
          params = { f0 = 55; gain = 0.35; };
          loop = true;
          # MONO, and this is load-bearing. pm_drone.dsp ends
          # `process = mono <: _, (_ : de.delay(...))` — two channels — and
          # libdragon's mixer plays a STEREO waveform across two ADJACENT
          # mixer channels. So a stereo bed started on channel 0 silently
          # occupies 0 AND 1, and anything else placed on 1 collides with
          # it: one of the two ends up with a sample buffer and no reader,
          # and mixer_poll asserts "samplebuffer_get: no reader to extend"
          # a few seconds into the boot.
          #
          # The stereo widening was a few milliseconds of delay on one side
          # that collapses to mono on a console speaker anyway, so this
          # costs nothing audible and halves the ROM cost.
          mono = true;
        };

        # ── The Kiln boot splash ──────────────────────────────────────────
        # A parody of the Nintendo 64's boot, and a publisher mark rather
        # than any one game's title screen — which is why the runtime half
        # is in the engine (engine/src/kiln/kiln_splash.h) and only the two
        # assets live here. Ganja Goblin can adopt it with four calls.
        kilnLogo = blenderLib.mkBlenderModel {
          name = "kiln_logo";
          script = "kiln_logo.py";
          # baseScale is MODEL UNITS PER BLENDER UNIT, not a "keep my units"
          # switch — flake.nix's own demoModel note says so ("a 1x1x1 Blender
          # unit cube; the default baseScale of 64 turns that into a 64-unit
          # cube"). At 1, this 3.1-unit-wide wordmark became 3 integer units
          # and collapsed into an unreadable grey slab on screen. Tiny3D
          # stores vertices as integers; sub-unit detail simply does not
          # survive. 64 keeps it consistent with every other model here, and
          # kiln_splash's camera is placed in the same units.
          baseScale = 64;
        };

        # The jingle. Its chord resolves 1.25 s in, which kiln_splash.c
        # times the logo's assembly and the screen flash to meet — picture
        # can be nudged a frame at runtime, audio cannot, so the sound is
        # the master here.
        kilnJingle = faust.mkBakedInstrument {
          name = "kilnjingle";
          src = ./dsp/kiln_jingle.dsp;
          sampleRate = 32000;
          duration = 4.0;
          params = { gain = 0.5; };
          # loop = true even though the jingle plays ONCE.
          #
          # A non-looping wav64 reaching its end crashed the mixer:
          # libdragon asserts "samplebuffer_get: no reader to extend" a
          # couple of seconds later, because a finished one-shot is still in
          # mixer_poll's rotation with nothing left to read. The looping
          # ambience bed never showed it, and that difference is the whole
          # clue. A looping sample simply never reaches that state.
          #
          # kiln_splash stops the channel at 3.8 s and the sample is 4.0 s,
          # so it is stopped before it would ever wrap — the loop flag costs
          # nothing audible and removes the end-of-sample path entirely.
          loop = true;
        };

        pmCentaurRig = ./PetaByte-Madness/assets/rig/machine_centaur.json;
        # ── The veil's first textured model: WIRED BUT NOT ENABLED ───────
        # machine_centaur.json already carries UVs and ten named material
        # groups, and three of the drop's textures are already indexed PNGs
        # named for them, which is why docs/ASSET_PIPELINE.md calls these "the
        # natural first real customer for pm_veil_bind_palette".
        #
        # Everything around them is built and verified: tools/veil_palette.py
        # bakes the cold/veiled pairs, assetLib.mkVeilTexture ships the CI4
        # sprite beside its .pal, pm_veil_load_palette loads all three (the debug
        # overlay reports `veil-pal 3/3`), and pm_veil_draw_model binds them per
        # material through Tiny3D's filterCb/tileCb.
        #
        # What does NOT work yet is the last link: giving these four groups
        # `tex0_decal` specs made the model render with NO TEXTURE SAMPLED. The
        # evidence is direct — a diagnostic build with a pure-GREEN cold palette
        # produced zero green pixels, so the TLUT never reaches the RDP — and it
        # was a visible REGRESSION, replacing the groups' vertex colours (3.84%
        # of frame pixels changed, max channel delta 248). Ruled out along the
        # way: the bake (veiled mean RGB is 11.1/0.9/0.6, correctly red), the
        # palette load, the combiner's TEX0 slot (moved from D to A for exactly
        # this reason — see f3d_inject's tex0_decal comment), and the UV range
        # (0..1 is one tile, which is what the working `checker` model uses).
        #
        # So the specs are left OUT rather than shipped broken. Turning the veil
        # on for the centaur is these four lines, once Tiny3D's handling of a
        # CI4 sprite in a material is settled:
        #
        #   "face=tex0_decal,tex=textures/mc_face.png,size=64,prim=1:1:1:1"
        #   "gore=tex0_decal,tex=textures/mc_gore.png,size=32,prim=1:1:1:1"
        #   "hull=tex0_decal,tex=textures/mc_plate.png,size=32,prim=1:1:1:1"
        #   "ribs=tex0_decal,tex=textures/mc_plate.png,size=32,prim=1:1:1:1"
        #
        # prim=1:1:1:1 is load-bearing, not decoration: RGB_MUL has no ONE
        # operand, so tex0_decal spells "multiply by one" as PRIM.
        pmCentaurModel = blenderLib.mkBlenderModel {
          name = "centaur";
          script = "centaur.py";
          scriptArgs = [ "--rig" "${pmCentaurRig}" ];
          materials = [
            "*=shade"
          ];
          textures = pmVeilTextureSet;
          animated = true;
          # No gameplay code calls t3d_model_bvh_query_frustum (same
          # reasoning as pmStorm's bvh=false below) — and a BVH computed
          # against this model's rest pose is dubious value anyway for a
          # skinned mesh whose bones move it away from that pose at runtime.
          bvh = false;
        };

        # Horner, rebuilt as a skinned, six-clip rig — same move as the
        # centaur above, for the same reason (pm_intake.c needs him to act,
        # not just stand there rigid). tools/blender/horner.py builds the
        # armature; PetaByte-Madness/tools/ph_rig_export.py produced the
        # committed JSON from ph_rig.py + ph_anim_clips.py (rig + mesh +
        # clips, self-tested), so this is a static file path exactly like
        # pmCentaurRig above, not a build-time derivation.
        pmHornerRig = ./PetaByte-Madness/assets/rig/horner.json;
        pmHornerModel = blenderLib.mkBlenderModel {
          name = "horner";
          script = "horner.py";
          scriptArgs = [ "--rig" "${pmHornerRig}" ];
          animated = true;
          bvh = false; # same reasoning as pmCentaurModel's bvh=false
        };

        # The MRI bay's pair of idle-animated robotic arms — hand-authored
        # (no external rig JSON; see tools/blender/lab_arms.py), same shape
        # droid.py's two-bone arms use.
        pmLabArmsModel = blenderLib.mkBlenderModel {
          name = "lab_arms";
          script = "lab_arms.py";
          animated = true;
          bvh = false; # always on screen in the lab, same reasoning as above
        };

        # The OBJ/glTF-sourced props: the island, its palms, the work
        # submarine, the guard mobs, the drone. One script with a --model
        # table (tools/blender/pm_props.py), the same shape goblin.py uses,
        # because each is the same three steps — read, colour, decimate.
        # Parsing lives in tools/blender/objkit.py, which imports no bpy and
        # is testable with a bare python3.
        #
        # See PetaByte-Madness/docs/ASSET_PIPELINE.md for why these do NOT go
        # through mkModel the way the demons do, and for the three mesh
        # defects in this drop that fail silently if unhandled.
        pmProp = name: blenderLib.mkBlenderModel {
          inherit name;
          script = "pm_props.py";
          # The whole assets directory, not one file: loach.obj resolves
          # loach.mtl as a sibling, and a store path for a single file has no
          # siblings.
          scriptArgs = [ "--model" name "--assets" "${./PetaByte-Madness/assets}" ];
          # No gameplay code calls t3d_model_bvh_query_frustum — see
          # pmStorm's bvh=false below for the precedent this reuses.
          bvh = false;
        };

        pmDemonModel = name: assetLib.mkModel {
          inherit name;
          src = ./PetaByte-Madness/assets/models/${name}.glb;
          dest = "models";
          ignoreMaterials = true;
          # Same reasoning as pmProp above: nothing queries it.
          bvh = false;
        };
        # The title skull. pm_screens.c has always looked for this and the
        # ROM never shipped it, so the title screen has been drawing its
        # text fallback — the branch was written to survive a missing asset
        # and did its job silently for the whole project.
        #
        # CI4, which pm_screens.c's own comment already specified: 64x64 at
        # 4bpp is 2 KB against a 4 KB TMEM budget, where RGBA16 would be
        # 8 KB and could not be loaded at all. A 16-entry palette is also
        # the veil's TLUT format, so this can later ride
        # pm_veil_bind_palette and bleed red as the filter rises.
        pmSkull = assetLib.mkSprite {
          name = "skull";
          src = ./PetaByte-Madness/assets/images/PetaByte_Madness64.png;
          dest = "sprites";
          format = "CI4";
        };

        # ── The veil's first real CI4 materials ──────────────────────────
        # docs/VEIL_DESIGN.md §1's palette swap is the game's headline
        # mechanic, and its TLUT half had never run: pm_veil_bind_palette,
        # _material_pass, _prim_alpha and _ramp_build all existed with zero call
        # sites because §8's "convert every material to CI4… this is the real
        # work" had no builder behind it. tools/veil_palette.py and
        # assetLib.mkVeilTexture are that builder.
        #
        # The centaur's three textures are the first customers because they are
        # already genuine CI4 source — indexed PNGs with 15, 7 and 5 colours,
        # which docs/ASSET_PIPELINE.md already calls "the natural first real
        # customer for pm_veil_bind_palette". Nothing had to be requantised.
        #
        # `veilClass = "demon"` on all three. The class name is about the VALUE
        # RATION, not about being an enemy: it means "owns true black and true
        # white", and the centaur is the subject of every shot he is in. The
        # environment gets "world" (a mid band) so that contrast stays his.
        # `phantom` — cold alpha 0 on every entry, i.e. not drawn at all with
        # the veil down — belongs to the four demons' bodies, and waits on them
        # growing UVs (see below).
        pmVeilTextures = map (t: assetLib.mkVeilTexture {
          name = t;
          src = ./PetaByte-Madness/assets/textures + "/${t}.png";
          veilClass = "demon";
        }) [ "mc_face" "mc_plate" "mc_gore" ];

        # mkBlenderModel's `textures` takes ONE derivation and copies
        # `$out/png/*.png` out of it, so the three are joined. symlinkJoin
        # rather than a fourth builder: they are already built, and merging
        # store paths is what it is for.
        pmVeilTextureSet = pkgs.symlinkJoin {
          name = "pm-veil-textures";
          paths = pmVeilTextures;
        };

        # `name` is the FILENAME, so it must match what main.c opens:
        # rom:/maps/pm_lab.map. It used to be "pm-lab-map", which shipped
        # maps/pm-lab-map.map — so kiln_map_load failed on every boot,
        # g_lab.brush_count stayed 0, and PM_SCREEN_PLAY had NO COLLISION WORLD
        # AT ALL. The player fell forever (measured: eye Y -24,193 six seconds
        # in) and PLAY rendered as a black screen with a working HUD over it.
        #
        # Nothing caught it because every layer degraded politely: kiln_map_load
        # returns non-zero rather than asserting, main.c's install is guarded on
        # that, and an empty clip world makes every trace report fraction 1
        # instead of failing. Three correct "survive a missing asset" decisions
        # composing into a silent one.
        #
        # Underscores, not hyphens, and not a decorative name: mkRawAsset has no
        # way to know what path the ROM will ask for, so the name IS the
        # contract. See pm_sfx.h for the same class of trap on the sfx path.
        # A Forge level baked into a ROM, so the LOAD path can be verified under
        # an emulator — which has no SD card, so `.#forge`'s normal storage
        # backend is unreachable there and its read path would otherwise only
        # ever be exercised on hardware. Generated from a committed .map by the
        # same host tool `./dev forge-push` uses, so this is not a special export:
        # it is the file the card would hold.
        forgeSeedFrg = pkgs.runCommand "forge-seed-frg"
          { nativeBuildInputs = [ pkgs.python3 ]; }
          ''
            mkdir -p $out
            python3 ${./tools/forge/frg.py} frommap ${./assets/oot_test.map} \
                    $out/LEVEL.frg
          '';

        # compress = 0 because kiln_store reads this with a plain fopen, not
        # asset_fopen: an mkasset-compressed payload would come back as its
        # container bytes and fail the CRC, which is a confusing way to discover
        # a compression setting.
        forgeSeedLevel = assetLib.mkRawAsset {
          name = "LEVEL";
          src = "${forgeSeedFrg}/LEVEL.frg";
          dest = "forge";
          extension = "frg";
          compress = 0;
        };

        pmLabMap = assetLib.mkRawAsset {
          name = "pm_lab";
          src = ./PetaByte-Madness/assets/pm_lab.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };

        # ── The night exterior ───────────────────────────────────────────
        # The sky and the sea, from tools/blender/pm_env.py. See
        # PetaByte-Madness/src/pm_env.h for what the game does with them and
        # why the horizon colour appears in three places.
        # The island hub and the lab, authored FOR the engine rather than
        # imported: flat walkable surfaces the AABB collision can match,
        # named sub-objects for the six dungeon gates, and metres as the
        # authoring unit. tools/blender/pm_world.py explains what the
        # OBJ-derived originals could not give the runtime.
        pmWorld = { name, materials ? [ "*=shade" ], textures ? null, bvh ? true }:
          blenderLib.mkBlenderModel {
            inherit name materials textures bvh;
            script = "pm_world.py";
          };

        # Named once, used both by mkPetabyteMadness's `assets` list below and
        # by the standalone `model-island` output — so a spot-check build and
        # the actual ROM can never independently drift on the terrain's
        # texture wiring the way `model-island = pmProp "island"` (the
        # retired OBJ-sourced island) used to silently point at the wrong
        # model entirely once pmWorld replaced it as the ROM's real source.
        pmIslandModel = pmWorld {
          name = "island";
          # The terrain object gets a real UV-blended texture (the band
          # atlas from tools/gen_textures.py); gates/tower fall through to
          # the untextured "*" wildcard, unchanged.
          materials = [
            "terrain=tex0_shade,tex=textures/terrain_bands.i8.png,size=32"
            "*=shade"
          ];
          inherit textures;
          # No BVH: nothing in PetaByte-Madness ever calls
          # t3d_model_bvh_query_frustum (see pmStorm's own bvh=false for the
          # precedent this reuses).
          bvh = false;
        };

        pmSkydome = blenderLib.mkBlenderModel {
          name = "skydome";
          script = "pm_env.py";
          # No BVH: the dome is drawn camera-centred with depth off, so it is
          # always entirely in frame and frustum-culling it can only cost.
          bvh = false;
        };
        pmStorm = blenderLib.mkBlenderModel {
          name = "storm";
          script = "pm_env.py";
          # Bolts are drawn unlit and are on screen for a handful of frames;
          # frustum-culling 66 triangles would cost more than it saves.
          bvh = false;
        };
        pmSea = blenderLib.mkBlenderModel {
          name = "sea";
          script = "pm_env.py";
          # tex0_shade is texel * shade, which is exactly the contract
          # pm_env.py's sea palette is authored against: the vertex colour is
          # a crest ceiling and the foam texture carves the troughs out of it.
          materials = [
            "water=tex0_shade,tex=textures/foam.i8.png,size=32"
          ];
          # gltf_to_t3d decodes the PNG at conversion time to learn its pixel
          # size, so the image has to be here even though the ROM ships the
          # .sprite (which rides in via `textures` in the assets list below).
          inherit textures;
          # Not just "nothing queries it" (pmStorm's reasoning) — a BVH
          # computed once at build time against the sea's rest pose would be
          # actively WRONG here, since pm_env.c's swell rewrites every
          # vertex's Y every frame and the bounds would no longer describe
          # where the mesh actually is.
          bvh = false;
        };

        # ── The theme ────────────────────────────────────────────────────
        # The game's main theme, authored as a string quartet, shipped TWICE
        # on purpose — see pm_music.h for what the game does with the pair.
        #
        # `pmTheme` is the score: MIDI -> XM (tools/midi_to_xm.py) -> XM64,
        # sequenced live by the RSP mixer. 4.7 KB, loops exactly, costs
        # almost nothing per frame.
        #
        # `pmThemeStream` is the recording: the mastered MP3 -> VADPCM
        # wav64, streamed from ROM. About 1.2 MB, and it is the arrangement
        # as it actually sounds rather than four synthesised waveforms.
        pmTheme = assetLib.mkMidiMusic {
          name = "petabyte";
          src = ./PetaByte-Madness/assets/music/petabyte.mid;
          converter = ./tools/midi_to_xm.py;
          songName = "PetaByte Madness";
        };
        # Mono and resampled to the ROM's own 32 kHz: the mixer would resample
        # anyway, and doing it at build time spends the cycles on the host.
        pmThemeStream = assetLib.mkSound {
          name = "petabyte_stream";
          src = ./PetaByte-Madness/assets/music/petabyte_theme.mp3;
          dest = "music";
          mono = true;
          resample = 32000;
          compress = 1; # vadpcm — the RSP-accelerated one
        };

        # ── The two new story-beat cues + the title-card FMV ────────────────
        # ostafterstart.mp3 and assets/mp4/audio.wav are the clean individual
        # sources; assets/music/intro-after-start.mp3 (not baked — reference
        # only) is the two of them concatenated, and ffprobe confirms their
        # durations sum to its exactly. See pm_narration.c / pm_lab.c for
        # where each actually plays.
        pmNarrationMusic = assetLib.mkSound {
          name = "narration_stream";
          src = ./PetaByte-Madness/assets/music/ostafterstart.mp3;
          dest = "music";
          mono = true;
          resample = 32000;
          compress = 1;
        };
        pmSurgeryOst = assetLib.mkSound {
          name = "surgery_ost";
          src = ./PetaByte-Madness/assets/mp4/audio.wav;
          dest = "music";
          mono = true;
          resample = 32000;
          compress = 1;
        };
        # intro.m1v has no audio track of its own (see pm_credits.h) — its
        # originally-intended companion is audio.wav above, now the surgery
        # OST instead. Plays silent until real matched audio exists.
        pmIntroVideo = assetLib.mkVideo {
          name = "intro";
          src = ./PetaByte-Madness/assets/mp4/intro.m1v;
        };
        # `debug` compiles pm_debug's overlay in; `jump` (null, or a PMScreen
        # name without the PM_SCREEN_ prefix) makes the ROM boot straight into
        # that screen.
        #
        # The jump exists because the interactive route to a screen needs a
        # working controller and there are two common situations without one:
        # `./dev shot`, which takes a single screenshot and has no input path
        # at all, and `./dev drive` on a machine where the uinput -> SDL -> ares
        # binding chain does not take (tools/n64-drive.sh's header is largely
        # about how fragile that chain is). A jump ROM plus `./dev shot` needs
        # neither, which makes it the reliable path for capturing a cutscene.
        mkPetabyteMadness = { debug, jump ? null, dd ? null, veilForce ? false,
                              cine ? false, shotAt ? null, ladder ? null,
                              cineLint ? false }:
          mkN64Rom {
          # Deliberately the same `name` in both variants: `name` is what
          # rom.nix's passthru.romFile is built from, and the Makefile emits
          # petabyte-madness.z64 either way. The two are separate store
          # paths because their makeFlags differ.
          name = "petabyte-madness";
          # Translated to -DPM_JUMP_SCREEN=PM_SCREEN_<jump> by
          # PetaByte-Madness/Makefile, which also errors out if it is asked for
          # without KILN_DEBUG rather than silently ignoring it.
          makeFlags = pkgs.lib.optional (jump != null) "PM_JUMP=${jump}"
                   ++ pkgs.lib.optional (dd != null) "PM_DD=${toString dd}"
                   ++ pkgs.lib.optional veilForce "PM_VEIL_FORCE=1"
                   # The cinematic debugger (PetaByte-Madness/src/pm_cine.h).
                   # `shotAt` and `ladder` each imply `cine` in the Makefile, so
                   # they do not have to be passed together here.
                   ++ pkgs.lib.optional cine "PM_CINE=1"
                   ++ pkgs.lib.optional (shotAt != null)
                        "PM_SHOT_AT=${toString shotAt}"
                   ++ pkgs.lib.optional (ladder != null)
                        "PM_SHOT_LADDER=${toString ladder}"
                   ++ pkgs.lib.optional cineLint "PM_CINE_LINT=1";
          src = ./PetaByte-Madness;
          romTitle = "PetaByte Madness";
          saveType = "eeprom4k"; # three profiles; see kiln_save.h's budget
          audioRate = 32000;     # cross-checked against pmDrone's bake rate
          debugConsole = debug;
          # `textures` ships the .sprite the sea's foam material names; the
          # model only carries the rom:/ path to it.
          assets = [ pmLabMap pmCentaurModel pmHornerModel pmLabArmsModel
                     pmDrone kilnLogo kilnJingle
                     pmTheme pmThemeStream
                     pmNarrationMusic pmSurgeryOst pmIntroVideo
                     pmSkydome pmSea pmStorm pmSkull textures ]
            ++ pmVeilTextures
            # The lab room ships as dank_lab.obj (via pmProp), not
            # pmWorld's procedural box — pm_lab.c's collision brushes,
            # player start pose, and note/MRI positions were all authored
            # against the OBJ's real bounding box from the start (see
            # pm_lab.h's LAB_X0..Z1), so this is the model that was always
            # meant to ship here. bvh=false and vertex-colour materials
            # both come from pmProp's existing defaults.
            ++ [ pmIslandModel (pmProp "dank_lab") ]
            ++ map pmProp [ "palms" "loach" "guard_cousin" ]
            ++ map pmDemonModel [ "imp" "hellhound" "gargoyle" "overlord" ];
        };
        petabyte-madness = mkPetabyteMadness { debug = false; };
        # The same ROM with pm_debug's state readout compiled in: screen,
        # the camera the scene was actually built from, near/far, and which
        # models resolved versus returned NULL. Every one of the four
        # defects behind the black-screen hunt would have been one glance
        # at this — see PetaByte-Madness/src/pm_debug.h. Kept out of the
        # shipping ROM so it pays nothing there.
        petabyte-madness-debug = mkPetabyteMadness { debug = true; };

        # One ROM per jumpable screen: `nix build .#pm-jump-intake` then
        # `./dev shot pm-jump-intake out.png 4` captures INTAKE four seconds in
        # with no controller involved. Names match PM_SCREEN_LIST
        # (PetaByte-Madness/src/pm_screens.h) lowercased, and the list here is
        # JUMPS[] in pm_screens.c — keep the two in step.
        # Each entry also picks the spatial overlay that screen is most worth
        # inspecting with (pm_debug.c's DD_SETS: 1 cam, 2 clip, 3 actors). A jump
        # ROM exists to be looked at, so having the relevant layer already on is
        # the useful default — and it is the only way an automated capture can
        # see it at all, since the cycle chord needs a controller.
        pmJumpScreens = [
          { s = "TITLE";     dd = 0; }   # a menu; lines would only obscure it
          { s = "FILE";      dd = 0; }
          { s = "NARRATION"; dd = 0; }   # 2D only, nothing spatial to draw
          { s = "LAB_CINE";  dd = 1; }   # keyframed camera -> the `cam` set
          { s = "LAB";       dd = 2; }   # hand-authored brushes -> `clip`
          { s = "INTAKE";    dd = 1; }
          { s = "CREDITS";   dd = 0; }
          { s = "SUB";       dd = 1; }
          { s = "BEACH";     dd = 1; }
          { s = "PLAY";      dd = 3; }   # who is actually spawned -> `actors`
        ];
        pmJumpRoms = pkgs.lib.listToAttrs (map (e: {
          name = "pm-jump-${pkgs.lib.toLower
                            (pkgs.lib.replaceStrings [ "_" ] [ "-" ] e.s)}";
          value = mkPetabyteMadness { debug = true; jump = e.s; inherit (e) dd; };
        }) pmJumpScreens);

        # The same jumps with the veil pinned on, for the A/B that shows the
        # palette swap. Only the screens with the centaur in them are worth it —
        # he is the only CI4-textured model, so he is the only place the TLUT
        # half of the effect can currently be seen at all.
        pmVeilRoms = pkgs.lib.listToAttrs (map (sc: {
          name = "pm-veil-${pkgs.lib.toLower sc}";
          value = mkPetabyteMadness {
            debug = true; jump = sc; dd = 0; veilForce = true;
          };
        }) [ "BEACH" "ATTRACT" "PLAY" ]);

        # ── The cinematic debugger's ROMs ──────────────────────────────────
        # Only the screens that ARE a keyframed shot. TITLE and FILE are menus
        # over one, PLAY is not a cutscene at all, and a transport with nothing
        # to transport is a timeline of a shot that is not playing.
        pmCineScreens = [ "NARRATION" "LAB_CINE" "INTAKE" "CREDITS" "SUB"
                          "BEACH" ];
        pmCineName = sc:
          pkgs.lib.toLower (pkgs.lib.replaceStrings [ "_" ] [ "-" ] sc);

        # `nix build .#pm-cine-intake` — boots into the shot with the transport
        # armed, the timeline drawn and the `cam` overlay on. This is the one to
        # reach for with a controller: L+R arms, START pauses, D-left/right
        # seeks, D-up/down walks the keys, Z detaches the free-fly and starts
        # printing a PMCamKey pose you can read straight off a screenshot.
        pmCineRoms = pkgs.lib.listToAttrs (map (sc: {
          name = "pm-cine-${pmCineName sc}";
          value = mkPetabyteMadness {
            debug = true; jump = sc; dd = 1; cine = true;
          };
        }) pmCineScreens);

        # `nix build .#pm-ladder-intake` — the same shot walked in eight
        # evenly-spaced rungs, each held for a fixed number of FRAMES, with the
        # shot time stamped on every frame. One boot yields the whole contact
        # sheet, and because the frames are self-labelling, extraction timing
        # drifting does not make the sheet ambiguous. `./dev cine` drives these.
        pmLadderRoms = pkgs.lib.listToAttrs (map (sc: {
          name = "pm-ladder-${pmCineName sc}";
          value = mkPetabyteMadness {
            debug = true; jump = sc; dd = 0; cine = true; ladder = 8;
          };
        }) pmCineScreens);

        # `nix build .#pm-cine-lint` — runs the camera validator over every shot
        # in PM_SHOT_LIST and draws the report, instead of running the game.
        # One `./dev shot pm-cine-lint out.png 8` reads the whole game's camera
        # health. The settle is long because the run plays each shot's setup(),
        # which preloads that shot's models.
        pm-cine-lint = mkPetabyteMadness { debug = true; cineLint = true; };

        # A NixOS-in-Docker image for collaborators: real Nix (so `nix
        # build`/`nix develop`/`./dev` work inside it against a cloned
        # checkout of this repo) plus the Claude Code CLI and Tailscale, for
        # private delivery/updates over a tailnet. See nix/dev-image.nix for
        # what it deliberately does and does not contain.
        dev-image = nixos-generators.nixosGenerate {
          inherit system;
          format = "docker";
          modules = [ ./nix/dev-image.nix ];
          specialArgs = { inherit claude-code-nix; };
        };
      in
      {
        packages = {
          inherit toolchain hello audio live-voice music-demo engine-demo ks-voice ks-baked sc64deployer unfloader n64Inst assets-demo actors-demo rooms-demo streamdb-demo camera-skel-demo clip-demo physics-demo map-demo event-demo oot-demo oot-demo-debug debug-demo interceptor-demo cinematic-demo texanim-demo fps bass-synth openworld-demo ganja-goblin board-demo petabyte-madness petabyte-madness-debug forge forge-dfs forge-selftest forge-selftest-sram;
          engine = kiln-engine;
          host-math = hostMath;
          streamdb = streamdb-emb;
          inherit textures;
          inherit dev-image;
        }
        # `nix build .#pm-jump-<screen>` — a debug ROM that boots straight into
        # one screen, for capture without a controller. `.#pm-veil-<screen>` is
        # the same with the veil pinned on. See mkPetabyteMadness.
        // forgeModeRoms
        // pmJumpRoms // pmVeilRoms
        # `.#pm-cine-<screen>` is the interactive transport, `.#pm-ladder-<screen>`
        # the deterministic contact sheet, `.#pm-cine-lint` the static camera
        # report. See PetaByte-Madness/src/pm_cine.h.
        // pmCineRoms // pmLadderRoms // { inherit pm-cine-lint; }
        # `nix build .#model-torus` converts one model on its own, which is the
        # fast loop when a shape comes out wrong: each derivation keeps its
        # intermediate glTF in share/gltf/, so geometry problems can be told
        # apart from material problems without rebuilding anything.
        // pkgs.lib.mapAttrs' (n: v: pkgs.lib.nameValuePair "model-${n}" v)
          testModels
        // {
          # Exposed so ./dev shot can resolve the emulator binary directly.
          ares-bin = pkgs.ares;
          libdragon = libdragon-sdk;
          tiny3d = tiny3d-sdk;
          model-goblin = goblinModel;
          model-interceptor = interceptorModel;
          model-droid = droidModel;
          model-alien = alienModel;
          model-quake-test = quakeTestModel;
          model-centaur = pmCentaurModel;
          model-kiln-logo = kilnLogo;
          model-island = pmIslandModel;
          model-palms = pmProp "palms";
          model-loach = pmProp "loach";
          model-drone = pmProp "drone";
          model-guard-cousin = pmProp "guard_cousin";
          model-dank-lab = pmProp "dank_lab";
          model-horner = pmHornerModel;
          model-lab-arms = pmLabArmsModel;
          model-gokart = gokartModel;
          model-bike = bikeModel;
          model-dank = goblinCast.dank;
          model-sparky = goblinCast.sparky;
          model-moss = goblinCast.moss;
          model-glimmer = goblinCast.glimmer;
          default = hello;
        };

        # Exposed so downstream flakes (the SSHitunneller! N64 port) can build
        # ROMs and voices against this pinned toolchain without vendoring it.
        lib = {
          inherit mkN64Rom;
          inherit (faust) mkFaustVoice mkBakedInstrument mkOfflineRenderer;
          inherit (assetLib) mkModel mkSprite mkFont mkSound mkMusic mkRawAsset mkStreamdb;
        };

        checks = {
          toolchain = import ./nix/checks/toolchain.nix { inherit pkgs toolchain; };
          rom-hello = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = hello;
            name = "hello";
          };
          rom-audio = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = audio;
            name = "audio";
          };
          rom-engine = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = engine-demo;
            name = "engine";
          };
          rom-assets-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = assets-demo;
            name = "assets-demo";
          };
          rom-actors-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = actors-demo;
            name = "actors-demo";
          };
          rom-rooms-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = rooms-demo;
            name = "rooms-demo";
          };
          rom-music-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = music-demo;
            name = "music";
          };
          rom-live-voice = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = live-voice;
            name = "live-voice";
          };
          rom-streamdb-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = streamdb-demo;
            name = "streamdb-demo";
          };
          rom-clip-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = clip-demo;
            name = "clip-demo";
          };
          rom-physics-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = physics-demo;
            name = "physics-demo";
          };
          rom-map-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = map-demo;
            name = "map-demo";
          };
          rom-event-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = event-demo;
            name = "event-demo";
          };
          rom-oot-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = oot-demo;
            name = "oot-demo";
          };
          rom-oot-demo-debug = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = oot-demo-debug;
            name = "oot-demo-debug";
          };
          rom-debug-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = debug-demo;
            name = "debug-demo";
          };
          rom-interceptor-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = interceptor-demo;
            name = "interceptor-demo";
          };
          rom-cinematic-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = cinematic-demo;
            name = "cinematic-demo";
          };
          rom-texanim-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = texanim-demo;
            name = "texanim-demo";
          };
          rom-fps = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = fps;
            name = "fps";
          };
          rom-bass-synth = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = bass-synth;
            name = "bass-synth";
          };
          rom-ganja-goblin = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = ganja-goblin;
            name = "ganja-goblin";
          };
          rom-board-demo = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = board-demo;
            name = "board-demo";
          };
          # rom.nix globs for *.z64 rather than taking a filename, which is
          # what makes this work for Forge: the Makefile emits `forge.z64` no
          # matter which flake attribute built it.
          rom-forge = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = forge;
            name = "forge";
          };
          rom-forge-selftest = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = forge-selftest;
            name = "forge-selftest";
          };
          rom-petabyte-madness = import ./nix/checks/rom.nix {
            inherit pkgs;
            rom = petabyte-madness;
            name = "petabyte-madness";
            # 16 MB stopped being enough the moment this ROM started
            # shipping a full-motion video (pmIntroVideo, a raw MPEG1
            # elementary stream — video.h's video_open makes no attempt
            # to compress it) alongside two multi-minute VADPCM tracks
            # (pmNarrationMusic, pmSurgeryOst). Real N64 carts shipped up to
            # 64 MB (Conker's Bad Fur Day among them) and SC64 supports the
            # same; 64 MB here is headroom for the FMV once its final cut
            # replaces the current placeholder, not a number picked to
            # exactly clear today's size.
            maxSize = 64 * 1024 * 1024;
          };
          kiln-asset = import ./nix/checks/kiln-asset.nix {
            inherit pkgs;
            streamdbSrc = streamdb;
            embeddedSrc = ./streamdb-embedded;
            engineSrc = ./engine;
          };
          streamdb = import ./nix/checks/streamdb.nix {
            inherit pkgs;
            streamdbSrc = streamdb;
            embeddedSrc = ./streamdb-embedded;
          };
          assets = import ./nix/checks/assets.nix {
            inherit pkgs;
            n64Inst = n64InstBase;
          };
          mapmaker-roundtrip = import ./nix/checks/mapmaker-roundtrip.nix {
            inherit pkgs;
          };
          # The host build of libdragon's fast math must compute what the
          # VR4300 computes, down to the tie-breaking rule.
          kiln-hostmath = import ./nix/checks/kiln-hostmath.nix {
            inherit pkgs hostMath;
          };
          # The rename is an invariant, not a state: reintroducing the old
          # name for this engine fails the build. See the check's own header.
          kiln-names = import ./nix/checks/kiln-names.nix {
            inherit pkgs;
            repo = ./.;
          };
          # Forge's own content round trip: the .FRG container, the host mirror
          # of kiln_voxel_boxes, and the .map emitter against the STRICT reader.
          # Held to mapmaker-roundtrip's standard by the same method, because the
          # two .map emitters have to agree about the same brush.
          forge-roundtrip = import ./nix/checks/forge-roundtrip.nix {
            inherit pkgs;
          };
          # The bpy-free geometry builder tests. Gated here because they are
          # the fastest feedback loop in the modelling pipeline and were, until
          # this entry existed, run by nobody — see the check's own header for
          # what that cost.
          blender-tests = import ./nix/checks/blender-tests.nix {
            inherit pkgs;
          };
          # The engine's pure-logic modules, compiled natively against
          # nix/checks/stub/ and asserted on. Found two real bugs on its first
          # run — see the check's own header.
          # engine/modules.mk's host tier must be exactly the set that
          # compiles natively — checked in both directions from one run.
          # The host font is libdragon's own, extracted; the header must be
          # current and the font must still be monospaced.
          kiln-font = import ./nix/checks/kiln-font.nix {
            inherit pkgs;
            libdragonSrc = libdragon;
            platHost = ./plat/host;
            fontTool = ./tools/font_extract.py;
          };
          # The 2D pass, rendered by the real kiln_gui.c through the host
          # software rasteriser and diffed against a committed capture. The
          # frame gate CLAUDE.md says needs a Wayland session — it does not,
          # once the renderer is software.
          kiln-gui = import ./nix/checks/kiln-gui.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
          };
          # kiln_widget's screens, rendered through the same host backend and
          # diffed against their committed captures.
          kiln-widget = import ./nix/checks/kiln-widget.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
            uipreviewSrc = ./tools/uipreview;
          };
          # The whole frame bracket — 3D pass, the seam, 2D pass — rendered
          # by the real kiln_engine.c and kiln_gui.c on the host.
          kiln-scene = import ./nix/checks/kiln-scene.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
          };
          # A real voxel mesh rendered with two combiners, which is how the
          # atlas-never-sampled defect became visible instead of arguable.
          kiln-voxmesh = import ./nix/checks/kiln-voxmesh.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
          };
          kiln-parity = import ./nix/checks/kiln-parity.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
          };
          kiln-logic = import ./nix/checks/kiln-logic.nix {
            inherit pkgs hostMath;
            engineSrc = ./engine;
            platHost = ./plat/host;
          };
          # The generated dimension headers must be current, and the generators
          # must still agree with the geometry the ROM actually ships.
          pm-gen-headers = import ./nix/checks/pm-gen-headers.nix {
            inherit pkgs;
          };
          # The cinematic camera validator, compiled natively and asserted on
          # in both directions, plus the guard that PM_SHOT_LIST names every
          # shot that exists.
          pm-cine = import ./nix/checks/pm-cine.nix {
            inherit pkgs hostMath;
            platHost = ./plat/host;
            pmSrc = ./PetaByte-Madness;
            # pm_camkey.h is a shim over the engine's kiln_camkey.h since Forge
            # became a fourth consumer of the curve.
            engineSrc = ./engine;
          };
          # The rig JSONs are generated too, and had no regeneration gate at
          # all — plus the clip lengths pm_intake.c restates as seconds, which
          # a diff cannot check because a consistently-regenerated file can
          # still be wrong for the game.
          pm-rigs = import ./nix/checks/pm-rigs.nix {
            inherit pkgs;
          };
          inherit hello audio live-voice music-demo engine-demo ks-voice ks-baked assets-demo actors-demo rooms-demo streamdb-demo clip-demo physics-demo map-demo event-demo oot-demo oot-demo-debug debug-demo interceptor-demo cinematic-demo texanim-demo fps bass-synth ganja-goblin board-demo petabyte-madness petabyte-madness-debug forge forge-dfs forge-selftest forge-selftest-sram;
        }
        # The mode-jump ROMs are gated too. They are the only way each of PAINT,
        # ENT, LIGHT, CAM and WALK gets built at all — a mode reachable only by a
        # chord is a mode nothing compiles unless something asks for it, and a
        # per-mode -D flag is exactly the sort of thing that rots unnoticed.
        // forgeModeRoms;

        apps = {
          # Iterate here. Per report §7, Ares is the reference emulator for
          # homebrew — but do NOT trust any emulator for audio work: the AI
          # clock-divider rate, RDRAM latency contention, and denormal/FPU
          # behaviour are exactly what emulators get subtly wrong.
          ares = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-ares" ''
              exec ${pkgs.ares}/bin/ares "$@"
            '');
          };
          # Cycle-accurate cross-check. Too slow for iteration; useful for A/B.
          cen64 = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-cen64" ''
              exec ${pkgs.cen64}/bin/cen64 "$@"
            '');
          };
          # The hardware path: upload, debugf stdio, IS-Viewer64, and the AUX
          # PC<->N64 channel. Needs nixosModules.n64-flashcart for USB perms.
          sc64 = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-sc64" ''
              exec ${sc64deployer}/bin/sc64deployer "$@"
            '');
          };
          unfloader = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-unfloader" ''
              exec ${unfloader}/bin/unfloader "$@"
            '');
          };
          dev = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-dev" ''
              exec ${pkgs.bash}/bin/bash "''${KILN_REPO:-$PWD}/dev" "$@"
            '');
          };
          # three.js .map maker (tools/mapmaker/). A dev-only web app run
          # outside the hermetic build — same authoring/outside-build vs.
          # consume/inside-build split as tools/blender-mcp/. Exports canonical
          # .map text the existing mkQuakeMapModel + kiln_map.c pipeline already
          # consumes; validate via ./dev map-validate.
          mapmaker = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-mapmaker" ''
              cd "''${KILN_REPO:-$PWD}/tools/mapmaker"
              exec ${pkgs.python3Minimal}/bin/python3 -m http.server 8000
            '');
          };
          # The animation editor. Same shape as mapmaker: a static page of
          # ES modules served by python's http.server, three.js vendored
          # once and shared between the two tools. `stage` first, because
          # the editor loads the real pipeline's own .gltf intermediate
          # rather than a special export — see tools/poser/stage.sh.
          poser = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-poser" ''
              cd "''${KILN_REPO:-$PWD}"
              if [ ! -f tools/poser/data/dank.gltf ]; then
                echo "staging models for the poser (first run)…"
                ./tools/poser/stage.sh dank
              fi
              cd tools/poser
              echo "poser on http://localhost:8001"
              exec ${pkgs.python3Minimal}/bin/python3 -m http.server 8001
            '');
          };
          poser-verify = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-poser-verify" ''
              exec ${pkgs.python3Minimal}/bin/python3 \
                "''${KILN_REPO:-$PWD}/tools/poser/verify.py" "$@"
            '');
          };
          map-validate = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-map-validate" ''
              exec ${pkgs.python3Minimal}/bin/python3 "''${KILN_REPO:-$PWD}/tools/mapmaker/validate.py" "$@"
            '');
          };
          # tools/blender-mcp/'s MCP server. The `mcp` package comes from
          # THIS flake's own pinned nixpkgs (flake.lock), not the
          # `nix shell --impure --expr 'import <nixpkgs> {}'` the README used
          # to document — that resolved against whatever channel NIX_PATH
          # happened to point at, unpinned and unaudited, the one thing this
          # repo's whole toolchain strategy otherwise refuses to accept (see
          # CLAUDE.md's "Hard-won facts"). server.py itself runs from the
          # live checkout (not copied into the store): it locates its sibling
          # tools/blender/{quake_map,godot_scene}.py via
          # `Path(__file__).resolve().parents[2]`, which only resolves
          # correctly against a real working tree — same KILN_REPO convention
          # as `dev`/`mapmaker`/`poser` above.
          blender-mcp = {
            type = "app";
            program = toString (pkgs.writeShellScript "kiln-blender-mcp" ''
              exec ${pkgs.python3.withPackages (ps: [ ps.mcp ])}/bin/python3 \
                "''${KILN_REPO:-$PWD}/tools/blender-mcp/server.py" "$@"
            '');
          };
        };

        devShells.default = pkgs.mkShell {
          packages = [
            toolchain
            n64Inst
            pkgs.gnumake
            pkgs.faust
            pkgs.ares
            pkgs.pkg-config
            # The capture loop's python: evdev drives tools/n64-input.py's
            # uinput gamepad (there is no way to reach a screen behind
            # "press start" without it), and pillow is what turns a
            # screenshot into the pixel statistics ./dev shot reports —
            # CLAUDE.md's own warning is that eyeballing the PNG has
            # already cost this project real time.
            (pkgs.python3.withPackages (ps: [ ps.evdev ps.pillow ]))
          ];

          # Bare metal: see nix/libdragon.nix.
          hardeningDisable = [ "all" ];

          N64_INST = n64Inst;
          N64_GCCPREFIX = toolchain;

          shellHook = ''
            echo "═══════════════════════════════════════════"
            echo " Kiln — N64 / ModRetro M64 development shell"
            echo " gcc        ${toolchain.passthru.version} (mips64-elf-)"
            echo " N64_INST   $N64_INST"
            echo " make · nix build .#hello · nix run .#ares -- <rom.z64>"
            echo "═══════════════════════════════════════════"
          '';
        };
      }) // {
      # ── NixOS modules (system-independent) ────────────────────────────
      nixosModules.n64-flashcart = import ./nix/udev.nix;
    };
}
