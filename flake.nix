# SPDX-License-Identifier: MPL-2.0
{
  description = "M64 — a Nix build system for Nintendo 64 / ModRetro M64 software";

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
  };

  outputs = { self, nixpkgs, flake-utils, libdragon, summercart64, tiny3d, streamdb }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # The cross toolchain. Owns the whole toolchain strategy; see the file.
        toolchain = import ./nix/toolchain.nix { inherit nixpkgs pkgs system; };

        # libdragon, installed into a store path used as $N64_INST.
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
        # because m64_asset.c includes <streamdb_embedded.h>.
        streamdb-emb = import ./nix/streamdb.nix {
          inherit pkgs toolchain;
          libdragon = libdragon-sdk;
          src = ./streamdb-embedded;
        };

        # Prefix stage 1: what the engine itself compiles against. Now
        # includes streamdb-emb so m64_asset.c can find streamdb_embedded.h.
        n64InstBase = import ./nix/n64-inst.nix {
          inherit pkgs;
          libdragon = libdragon-sdk;
          extraLibs = [ tiny3d-sdk streamdb-emb ];
        };

        # The M64 engine — 3D on Tiny3D, 2D GUI on rdpq, asset layer on
        # streamdb-embedded.
        m64-engine = import ./nix/engine.nix {
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
        # m64_asset accessor: m64_asset_model (cube.t3dm), m64_asset_sprite
        # (logo.sprite), and m64_asset_load on a raw level-layout blob.
        #
        # IMPORTANT: compress = 0 across the board. m64_asset_load returns
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
        # A level-layout blob: 4-byte magic 'M64L' + u32 spawn-count + N vec3
        # spawn points. Generated deterministically (no binary checked in).
        introLevel = pkgs.runCommandLocal "intro-level.bin" {
          nativeBuildInputs = [ pkgs.python3 ];
        } ''
          python3 -c '
          import struct, sys
          spawns = [(0,0,0),(10,0,10),(-10,0,10),(0,5,0)]
          with open(sys.argv[1], "wb") as f:
              f.write(b"M64L")
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
        # Loose Quake .map shipped to rom:/maps/ for m64_map at runtime.
        quakeMap = assetLib.mkRawAsset {
          name = "quake-test-map";
          src = ./assets/quake_test.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # Two-room + enemies test map for examples/oot-demo. Same raw-asset
        # path as quakeMap so m64_map reads it via rom:/maps/oot_test.map.
        ootMap = assetLib.mkRawAsset {
          name = "oot-test-map";
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
        textures = assetLib.mkTextures { name = "m64"; };

        # ── Test geometry ─────────────────────────────────────────────
        # Each model isolates one class of renderer bug; tools/blender/models.py
        # documents which. Untextured models go through the `shade` preset,
        # which reproduces libdragon's RDPQ_COMBINER_SHADE exactly — the same
        # combiner m64_scene_begin() already sets, so they compose with
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
        # Blender-authoring path (m64_skel.h's runtime side is exercised
        # separately by examples/camera-skel-demo's hand-authored rig).
        # tools/blender/goblin.py documents why every part is rigidly bound
        # to exactly one bone.
        goblinModel = blenderLib.mkBlenderModel {
          name = "goblin";
          script = "goblin.py";
          animated = true;
        };

        # The hero prop: not a test shape, but a piece of content authored the
        # way a game's content is — one silhouette from six interpenetrating
        # parts, shaded entirely by COLOR_0 through the same `shade` combiner
        # m64_scene_begin() already sets, so it composes with hand-built
        # geometry in one pass and needs no TMEM. tools/blender/interceptor.py
        # documents the orientation and the budget.
        interceptorModel = blenderLib.mkBlenderModel {
          name = "interceptor";
          script = "interceptor.py";
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
        # Same mkRawAsset path as quakeMap / ootMap so m64_map reads it via
        # rom:/maps/hangar.map at runtime. compress=0 because m64_map_load
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
        # runtime via m64_map (same raw-asset path as ootMap / hangarMap).
        fpsMap = assetLib.mkRawAsset {
          name = "fps-level-map";
          src = ./assets/fps_level.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        # Per-room maps for the multi-room streaming FPS.
        fpsRoom0 = assetLib.mkRawAsset {
          name = "fps-room0-map";
          src = ./assets/fps_room0.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        fpsRoom1 = assetLib.mkRawAsset {
          name = "fps-room1-map";
          src = ./assets/fps_room1.map;
          dest = "maps";
          extension = "map";
          compress = 0;
        };
        fpsRoom2 = assetLib.mkRawAsset {
          name = "fps-room2-map";
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
          extraLibs = [ tiny3d-sdk m64-engine streamdb-emb ];
        };

        # The ROM builder. Projects bring a Makefile; this supplies the
        # hermetic environment. See nix/rom.nix for why.
        mkN64Rom = import ./nix/rom.nix {
          inherit pkgs toolchain n64Inst;
        };

        hello = mkN64Rom {
          name = "hello";
          src = ./examples/hello;
          romTitle = "M64 Hello";
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
          romTitle = "M64 Audio";
          assets = [ ks-baked ];
          audioRate = 32000;
        };

        # Live Faust voice + baked instrument A/B comparison (report Stage 2).
        # Links the ks-voice MIPS object into the ROM and renders it
        # sample-by-sample on the VR4300, mixed with the RSP mixer output.
        live-voice = mkN64Rom {
          name = "live-voice";
          src = ./examples/live-voice;
          romTitle = "M64 Live Voice";
          assets = [ ks-baked ];
          audioRate = 32000;
          makeFlags = [ "FAUST_VOICE=${ks-voice}/lib/ksvoice.o" ];
        };

        # 3D + 2D GUI worked example.
        engine-demo = mkN64Rom {
          name = "engine";
          src = ./examples/engine;
          romTitle = "M64 Engine";
        };

        # Open-world streaming demo: scratch allocator, refcounted cache,
        # tile residency manager, LOD selector, two-pass renderer.
        openworld-demo = mkN64Rom {
          name = "openworld-demo";
          src = ./examples/openworld-demo;
          romTitle = "M64 Open World";
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
          romTitle = "M64 Music";
          assets = [ test-music ];
          audioRate = 32000;
        };

        sc64deployer = import ./nix/tools/sc64deployer.nix {
          inherit pkgs;
          src = summercart64;
        };

        # Phase A verification: a ROM that loads one of each converted asset
        # kind — proves gltf_to_t3d, mksprite and audioconv64 actually run,
        # not just that the Nix glue around them evaluates.
        assets-demo = mkN64Rom {
          name = "assets-demo";
          src = ./examples/assets-demo;
          romTitle = "M64 Assets";
          assets = [ demoModel demoSprite demoSound ];
          audioRate = 32000;
        };

        # Phase B verification: the actor system (engine/src/m64/m64_actor.*)
        # with one profile per category that matters here — spawn, handle-based
        # despawn, an actor despawning itself mid-update, and the fixed
        # category draw order all exercised in one ROM.
        actors-demo = mkN64Rom {
          name = "actors-demo";
          src = ./examples/actors-demo;
          romTitle = "M64 Actors";
        };

        # Scene/room streaming — a 2×2 grid of rooms, camera starts in room A.
        # The m64_room module loads the room under the camera and its
        # neighbours; HUD reports current room + loaded count.
        rooms-demo = mkN64Rom {
          name = "rooms-demo";
          src = ./examples/rooms-demo;
          romTitle = "M64 Rooms";
        };

        # Phase B completion: m64_camera (OoT-style spring-arm follow) +
        # m64_skel (skeletal animation, idle/swing blend) + m64_audio
        # (footstep SFX on distance travelled) together in one ROM, driving
        # the m64_actor player already exercised by actors-demo.
        camera-skel-demo = mkN64Rom {
          name = "camera-skel-demo";
          src = ./examples/camera-skel-demo;
          romTitle = "M64 Camera Skel";
          assets = [ skelModel demoSound ];
          audioRate = 32000;
        };

        # Phase C verification: same three asset kinds as assets-demo, but
        # loaded from a single StreamDB container mounted at boot via
        # m64_asset_open. Exercises m64_asset_model (the patched
        # t3d_model_load_buf path), m64_asset_sprite (sprite_load_buf), and
        # m64_asset_load on a raw level blob, plus m64_asset_count and
        # m64_asset_find_suffix.
        streamdb-demo = mkN64Rom {
          name = "streamdb-demo";
          src = ./examples/streamdb-demo;
          romTitle = "M64 StreamDB";
          assets = [ demoStreamdb ];
        };

        # Phase C step 1: m64_input (deadzoned joypad wrapper with button
        # edges) + m64_clip (swept-AABB-vs-brushes collision with iterative
        # SlideMove). One player box pushed around a 5-brush room; the box
        # slides along walls, HUD reports the last trace's fraction / normal /
        # surface. No assets, no actors — the proof stays focused on the
        # collision primitive.
        clip-demo = mkN64Rom {
          name = "clip-demo";
          src = ./examples/clip-demo;
          romTitle = "M64 Clip";
          assets = [ demoSound stepSound ];
        };

        # Phase E: m64_room brush auto-install + m64_clip broadphase toggle
        # + m64_physics HL2-style rigid bodies. One room (floor + 4 walls,
        # brushes auto-installed via m64_room); 6 dynamic crate bodies fall,
        # stack, rest, sleep; A punts the nearest crate in a forward cone
        # (gravity-gun feel); D-pad toggles PHYS ON/OFF and BP ON/OFF; HUD
        # shows the last trace's brush count so the broadphase win is visible.
        physics-demo = mkN64Rom {
          name = "physics-demo";
          src = ./examples/physics-demo;
          romTitle = "M64 Physics";
        };

        # Phase C step 2: m64_dict + m64_map. Loads assets/quake_test.map,
        # parses it into brushes + face quads, and spawns the player at the
        # info_player_start entity by reading "origin" from the M64Dict.
        map-demo = mkN64Rom {
          name = "map-demo";
          src = ./examples/map-demo;
          romTitle = "M64 Map";
          assets = [ quakeMap ];
        };

        # Phase 4: m64_event. A switch actor posts DOOR_OPEN with a 500 ms
        # delay; the door actor's event callback rotates it open. HUD shows
        # the queued-event count so the 500 ms gap is visible.
        event-demo = mkN64Rom {
          name = "event-demo";
          src = ./examples/event-demo;
          romTitle = "M64 Event";
        };

        # Phase 6: the OoT + id Tech 4 integration proof. A player actor
        # (m64_player locomotion) walks an oot_test.map room, slides via
        # m64_clip, Z-targets enemies (m64_target + camera TARGETING mode),
        # and emits footstep SFX through m64_event + m64_sound shaders.
        oot-demo = mkN64Rom {
          name = "oot-demo";
          src = ./examples/oot-demo;
          romTitle = "M64 OoT";
          assets = [ ootMap stepSound ];
        };

        # The single-screen showcase: title + 3-mode flight + engine streaks +
        # credit HUD. Loads the hand-authored Interceptor starfighter through
        # the same mkBlenderModel path tools/blender/interceptor.py documents.
        interceptor-demo = mkN64Rom {
          name = "interceptor-demo";
          src = ./examples/interceptor-demo;
          romTitle = "M64 Interceptor";
          assets = [ interceptorModel demoSound test-music ];
          audioRate = 32000;
        };

        # texanim-demo: exercises m64_texanim (UV scroll, flipbook, palette,
        # offscreen) and m64_vanim (procedural deform, morph blending, RSP
        # vertex FX). All geometry is hand-built — no asset pipeline needed.
        texanim-demo = mkN64Rom {
          name = "texanim-demo";
          src = ./examples/texanim-demo;
          romTitle = "M64 TexAnim";
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
          romTitle = "M64 Cinematic";
          assets = [
            interceptorModel goblinModel droidModel alienModel
            hangarMap demoSound stepSound cine-music
          ];
          audioRate = 32000;
        };

        # A minimal playable first-person shooter. First-person camera
        # (m64_fpscam), hitscan weapon (m64_weapon), enemy actors that chase
        # the player, HUD with crosshair + health + ammo. The FPS level is a
        # Quake .map loaded at runtime via m64_map.
        fps = mkN64Rom {
          name = "fps";
          src = ./examples/fps;
          romTitle = "M64 FPS";
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
          romTitle = "M64 Bass Synth";
          assets = bassWavFlat;
          audioRate = 32000;
        };
      in
      {
        packages = {
          inherit toolchain hello audio live-voice music-demo engine-demo ks-voice ks-baked sc64deployer n64Inst assets-demo actors-demo rooms-demo streamdb-demo camera-skel-demo clip-demo physics-demo map-demo event-demo oot-demo interceptor-demo cinematic-demo texanim-demo fps bass-synth openworld-demo;
          engine = m64-engine;
          streamdb = streamdb-emb;
          inherit textures;
        }
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
          m64-asset = import ./nix/checks/m64-asset.nix {
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
          inherit hello audio live-voice music-demo engine-demo ks-voice ks-baked assets-demo actors-demo rooms-demo streamdb-demo clip-demo physics-demo map-demo event-demo oot-demo interceptor-demo cinematic-demo texanim-demo fps bass-synth;
        };

        apps = {
          # Iterate here. Per report §7, Ares is the reference emulator for
          # homebrew — but do NOT trust any emulator for audio work: the AI
          # clock-divider rate, RDRAM latency contention, and denormal/FPU
          # behaviour are exactly what emulators get subtly wrong.
          ares = {
            type = "app";
            program = toString (pkgs.writeShellScript "m64-ares" ''
              exec ${pkgs.ares}/bin/ares "$@"
            '');
          };
          # Cycle-accurate cross-check. Too slow for iteration; useful for A/B.
          cen64 = {
            type = "app";
            program = toString (pkgs.writeShellScript "m64-cen64" ''
              exec ${pkgs.cen64}/bin/cen64 "$@"
            '');
          };
          # The hardware path: upload, debugf stdio, IS-Viewer64, and the AUX
          # PC<->N64 channel. Needs nixosModules.n64-flashcart for USB perms.
          sc64 = {
            type = "app";
            program = toString (pkgs.writeShellScript "m64-sc64" ''
              exec ${sc64deployer}/bin/sc64deployer "$@"
            '');
          };
          dev = {
            type = "app";
            program = toString (pkgs.writeShellScript "m64-dev" ''
              exec ${pkgs.bash}/bin/bash "''${M64_REPO:-$PWD}/dev" "$@"
            '');
          };
          # three.js .map maker (tools/mapmaker/). A dev-only web app run
          # outside the hermetic build — same authoring/outside-build vs.
          # consume/inside-build split as tools/blender-mcp/. Exports canonical
          # .map text the existing mkQuakeMapModel + m64_map.c pipeline already
          # consumes; validate via ./dev map-validate.
          mapmaker = {
            type = "app";
            program = toString (pkgs.writeShellScript "m64-mapmaker" ''
              cd "''${M64_REPO:-$PWD}/tools/mapmaker"
              exec ${pkgs.python3Minimal}/bin/python3 -m http.server 8000
            '');
          };
          map-validate = {
            type = "app";
            program = toString (pkgs.writeShellScript "m64-map-validate" ''
              exec ${pkgs.python3Minimal}/bin/python3 "''${M64_REPO:-$PWD}/tools/mapmaker/validate.py" "$@"
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
          ];

          # Bare metal: see nix/libdragon.nix.
          hardeningDisable = [ "all" ];

          N64_INST = n64Inst;
          N64_GCCPREFIX = toolchain;

          shellHook = ''
            echo "═══════════════════════════════════════════"
            echo " M64 — N64 / ModRetro M64 development shell"
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
