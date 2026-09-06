/* SPDX-License-Identifier: MIT
 *
 * forge_io.c — the .FRG working format, and the Quake .map the repo consumes.
 *
 * Two outputs with two different jobs, and conflating them would cost one of
 * them:
 *
 *   .FRG   the editor's own state: voxels and the texture atlas, RLE'd, with
 *          kiln_store's CRC over the whole payload. LOSSLESS across sessions.
 *          It has to be, because the greedy box reduction is one-way — a .map
 *          round-tripped back into blocks would come home as boxes, and the
 *          next save would look completely different in a diff.
 *
 *   .MAP   the derived artifact: greedy boxes as Quake brushes, in the exact
 *          dialect this repo already pins. Emitted as plain ASCII BY THE ROM,
 *          which is the decision that makes the whole loop cheap: `./dev
 *          forge-pull` becomes a copy plus `./dev map-validate` instead of a
 *          bespoke binary decoder that would need its own tests and its own
 *          drift.
 *
 * ── The .map dialect is not improvised ─────────────────────────────────
 *
 * Standard idTech2 format, never Valve 220 — tools/blender/quake_map.py:32
 * hard-fails on the bracketed UV axes rather than mis-reading them. Fifteen
 * tokens per face line, all required, trailing `0 0 0 1 1` which the engine
 * counts and ignores. Integers only, because kiln_map.c truncates to int16.
 *
 * The WINDING is the part that is easy to get wrong and expensive to discover:
 * p1,p2,p3 are clockwise seen from OUTSIDE, and the outward normal is
 * cross(p3-p1, p2-p1) — NOT the obvious cross(p2-p1, p3-p1). The console parser
 * is winding-independent (it componentwise min/max's the plane points, so
 * anything loads), but quake_map.py's CSG is not, and that is what
 * `./dev map-validate` and mkQuakeMapModel run. So a wrong winding produces a
 * file that plays correctly and cannot be turned into geometry — and
 * assets/oot_test.map is exactly that file today, which is why
 * mapmaker-roundtrip seeds from quake_test.map instead.
 *
 * The six faces below are therefore transcribed from tools/mapmaker/src/mapio.js's
 * `aabbFaces`, in its order, so the two emitters agree byte for byte on the same
 * brush. That is deliberate duplication of six lines across a language boundary;
 * the alternative is two conventions that agree until they do not.
 */
#include <stdio.h>
#include <string.h>

#include "forge.h"
#include "forge_map.h"

/* ── .FRG payload ──────────────────────────────────────────────────────*/

/* Bumped to 2 when entities, the light rig and the camera keys joined the
 * payload. The version is CHECKED on read (kiln_store_read refuses a mismatch),
 * so an old .FRG on a card does not load as a new one with the tail read as
 * garbage — it refuses, says EVERSION, and the editor reports which step. That
 * is the whole reason the header carries a version rather than the format being
 * "whatever the current ROM writes". */
#define FRG_VERSION 2

/* A payload buffer big enough for the worst case: 24 chunks whose RLE does not
 * compress at all (4096 blocks -> 8192 bytes as count/value pairs), plus the
 * atlas. 24 * 8195 + 4160 = ~200 KB. Static rather than malloc'd, and sized for
 * the pathological case rather than the typical one, because the failure mode of
 * guessing low is a save that refuses at the moment it is needed most. */
#define FRG_MAX_BYTES (KILN_VOXEL_MAX_CHUNKS * (3 + 2 * KILN_VOXEL_CHUNK_BLOCKS) \
                       + KILN_VOXATLAS_SIDE * KILN_VOXATLAS_SIDE                \
                       + 2 * KILN_VOXATLAS_COLOURS * 2                         \
                       + FORGE_MAX_ENTS * 32                                  \
                       + FORGE_MAX_KEYS * 32                                  \
                       + 128)
static uint8_t g_frg[FRG_MAX_BYTES];

/* Text buffer for the .map. 512 brushes * 8 lines * ~64 chars. */
/* Brushes plus point entities. 64 entities at ~120 bytes each is 8 KB on top of
 * the brush text. */
#define MAP_MAX_BYTES (512 * 8 * 64 + FORGE_MAX_ENTS * 160)
/* The generated light header and the PMCamKey table, each written as its own
 * file so the two can be consumed independently — a level's lighting and its
 * cutscene are edited by different people at different times. */
#define AUX_MAX_BYTES 8192
static char g_aux[AUX_MAX_BYTES];
static char g_map[MAP_MAX_BYTES];

static uint32_t put_u8(uint8_t *p, uint32_t at, uint8_t v) { p[at] = v; return at + 1; }

static uint32_t put_u16(uint8_t *p, uint32_t at, uint16_t v)
{
    /* Big-endian throughout, matching the target, and tools/forge/frg.py is
     * told so explicitly rather than left to guess from one sample file. */
    p[at] = (uint8_t)(v >> 8); p[at + 1] = (uint8_t)v; return at + 2;
}

static uint32_t put_f32(uint8_t *p, uint32_t at, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    p[at] = (uint8_t)(bits >> 24); p[at + 1] = (uint8_t)(bits >> 16);
    p[at + 2] = (uint8_t)(bits >> 8); p[at + 3] = (uint8_t)bits;
    return at + 4;
}

static uint32_t get_u16(const uint8_t *p, uint32_t at, uint16_t *out)
{
    *out = (uint16_t)((p[at] << 8) | p[at + 1]); return at + 2;
}

static uint32_t get_f32(const uint8_t *p, uint32_t at, float *out)
{
    uint32_t bits = ((uint32_t)p[at] << 24) | ((uint32_t)p[at + 1] << 16) |
                    ((uint32_t)p[at + 2] << 8) | p[at + 3];
    memcpy(out, &bits, 4);
    return at + 4;
}

/* Run-length encoding over a chunk's 4096 bytes. A voxel chunk is mostly one
 * value — that is what makes it a voxel chunk — so RLE takes a solid 16-cube
 * from 4096 bytes to 32 and a typical room's chunk to a few hundred. Runs cap at
 * 255 so a length is one byte. */
static uint32_t rle_encode(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t at)
{
    uint32_t i = 0;
    while (i < n) {
        uint8_t v = src[i];
        uint32_t run = 1;
        while (i + run < n && src[i + run] == v && run < 255) run++;
        at = put_u8(dst, at, (uint8_t)run);
        at = put_u8(dst, at, v);
        i += run;
    }
    return at;
}

static uint32_t rle_decode(const uint8_t *src, uint32_t at, uint32_t src_len,
                           uint8_t *dst, uint32_t n)
{
    uint32_t o = 0;
    while (o < n) {
        /* A truncated stream must stop rather than read past the payload. The
         * CRC already caught corruption; this catches a payload that is
         * self-consistently wrong, which a checksum cannot. */
        if (at + 1 >= src_len) return at;
        uint8_t run = src[at++];
        uint8_t v = src[at++];
        if (!run) return at;                  /* a zero run would never terminate */
        for (uint8_t k = 0; k < run && o < n; k++) dst[o++] = v;
    }
    return at;
}

int forge_io_save(Forge *f)
{
    uint32_t at = 0;

    at = put_f32(g_frg, at, f->world.offset.v[0]);
    at = put_f32(g_frg, at, f->world.offset.v[1]);
    at = put_f32(g_frg, at, f->world.offset.v[2]);
    at = put_u16(g_frg, at, (uint16_t)kiln_voxel_chunk_count(&f->world));

    for (int s = kiln_voxel_slot_first(&f->world); s >= 0;
             s = kiln_voxel_slot_next(&f->world, s)) {
        const KilnVoxelChunk *c = &f->world.chunks[s];
        at = put_u8(g_frg, at, c->cx);
        at = put_u8(g_frg, at, c->cy);
        at = put_u8(g_frg, at, c->cz);
        at = rle_encode(c->blocks, KILN_VOXEL_CHUNK_BLOCKS, g_frg, at);
    }

    /* The atlas rides in the same blob rather than a sibling file. A level and
     * the tiles it references are one artifact: shipping them separately means a
     * .FRG that loads with the wrong textures, which renders perfectly and is
     * wrong — the exact failure mkVeilTexture exists offline to prevent. */
    memcpy(&g_frg[at], f->atlas.index, sizeof f->atlas.index);
    at += sizeof f->atlas.index;
    for (int st = 0; st < 2; st++)
        for (int i = 0; i < KILN_VOXATLAS_COLOURS; i++)
            at = put_u16(g_frg, at, f->atlas.tlut[st][i]);

    /* Entities. Positions as floats rather than block indices: an entity is a
     * point in the world, not a cell, and quantising it to the block grid on
     * every save would walk it towards a corner one round trip at a time. */
    at = put_u16(g_frg, at, (uint16_t)f->ent_count);
    for (int i = 0; i < f->ent_count; i++) {
        const ForgeEnt *e = &f->ents[i];
        at = put_f32(g_frg, at, e->pos.v[0]);
        at = put_f32(g_frg, at, e->pos.v[1]);
        at = put_f32(g_frg, at, e->pos.v[2]);
        at = put_u16(g_frg, at, (uint16_t)e->angle);
        at = put_u8(g_frg, at, e->classname);
        for (int k = 0; k < FORGE_EPAIRS; k++)
            at = put_u16(g_frg, at, (uint16_t)e->epair[k]);
    }

    /* The light rig. Angles, not the derived direction vectors: the angles are
     * what the editor edits, and round-tripping through a normalised vector and
     * back would drift the value under the cursor. */
    at = put_f32(g_frg, at, f->key_yaw);
    at = put_f32(g_frg, at, f->key_pitch);
    at = put_f32(g_frg, at, f->fill_yaw);
    at = put_f32(g_frg, at, f->fill_pitch);
    at = put_u8(g_frg, at, f->key_level);
    at = put_u8(g_frg, at, f->fill_level);
    at = put_u8(g_frg, at, f->ambient);
    at = put_u8(g_frg, at, (uint8_t)f->fog_on);
    at = put_u8(g_frg, at, (uint8_t)f->clear_idx);
    at = put_f32(g_frg, at, f->fog_near);
    at = put_f32(g_frg, at, f->fog_far);

    /* Camera keys. */
    at = put_u16(g_frg, at, (uint16_t)f->key_count);
    at = put_f32(g_frg, at, f->cine_duration);
    at = put_u8(g_frg, at, (uint8_t)f->cine_loop);
    for (int i = 0; i < f->key_count; i++) {
        at = put_f32(g_frg, at, f->keys[i].t);
        for (int a = 0; a < 3; a++) at = put_f32(g_frg, at, f->keys[i].eye.v[a]);
        for (int a = 0; a < 3; a++) at = put_f32(g_frg, at, f->keys[i].look.v[a]);
    }

    f->last_action = "save";
    f->last_store = kiln_store_write(FORGE_LEVEL_NAME, FRG_VERSION, g_frg, at);
    if (f->last_store != KILN_STORE_OK) return f->last_store;

    /* ── The derived .map ──────────────────────────────────────────────*/
    int nb = kiln_voxel_boxes(&f->world, f->boxes, FORGE_MAX_BOXES, NULL);
    if (nb < 0) { f->box_overflow = -nb; nb = kiln_voxel_boxes(&f->world, f->boxes, FORGE_MAX_BOXES - 1, NULL); }
    if (nb < 0) nb = 0;
    f->boxes_used = (uint32_t)nb;

    int n = 0;
    n += snprintf(g_map + n, (size_t)(MAP_MAX_BYTES - n), "{\n\"classname\" \"worldspawn\"\n");

    for (int i = 0; i < nb && n < MAP_MAX_BYTES - 512; i++) {
        int x0 = (int)f->boxes[i].mins.v[0], y0 = (int)f->boxes[i].mins.v[1], z0 = (int)f->boxes[i].mins.v[2];
        int x1 = (int)f->boxes[i].maxs.v[0], y1 = (int)f->boxes[i].maxs.v[1], z1 = (int)f->boxes[i].maxs.v[2];

        /* One texture name per block type. Names, not indices, because the
         * .map format carries a name and every other tool in the chain reads
         * it as one: quake_map.py turns it into a Blender material and
         * kiln_surface keys gameplay off the brush's surface id. */
        char tex[16];
        snprintf(tex, sizeof tex, "FORGE%d", f->boxes[i].surface);

        /* The winding lives in tools/schema/level_vocab.json and is emitted
         * by forge_map_emit_box, which compiles natively so a gate can assert
         * on it. It used to be transcribed here, in a snprintf format string,
         * with a comment asking the next reader not to tidy it. */
        const int mins[3] = { x0, y0, z0 };
        const int maxs[3] = { x1, y1, z1 };
        n += forge_map_emit_box(g_map + n, (size_t)(MAP_MAX_BYTES - n),
                                mins, maxs, tex);
    }
    n += snprintf(g_map + n, (size_t)(MAP_MAX_BYTES - n), "}\n");

    /* The placed entities. */
    n += forge_ent_emit(f, g_map + n, MAP_MAX_BYTES - n);

    /* A spawn point, ONLY if the author did not place one. A .map with no
     * info_player_start loads and then has nowhere to put the player, and
     * map-demo's own fallback for that is a hardcoded origin — which is a
     * silent disagreement with the level. But overwriting an author's own spawn
     * with the camera position would be worse, so it is a fallback and not a
     * fixture. */
    int has_spawn = 0;
    for (int i = 0; i < f->ent_count; i++)
        if (strcmp(forge_ent_classname(f->ents[i].classname),
                   "info_player_start") == 0) has_spawn = 1;
    if (!has_spawn)
        n += snprintf(g_map + n, (size_t)(MAP_MAX_BYTES - n),
                      "{\n\"classname\" \"info_player_start\"\n"
                      "\"origin\" \"%d %d %d\"\n}\n",
                      (int)f->fly_pos.v[0], (int)f->fly_pos.v[1],
                      (int)f->fly_pos.v[2]);

    int text_st = kiln_store_write_text(FORGE_LEVEL_NAME, "MAP", g_map);

    /* The light header and the camera table, as their own text files. Written
     * BEFORE the status is decided below so a failure on either still logs. */
    forge_light_emit(f, g_aux, AUX_MAX_BYTES);
    int lt_st = kiln_store_write_text(FORGE_LEVEL_NAME, "H", g_aux);

    int cam_st = KILN_STORE_OK;
    if (f->key_count > 0) {
        /* Re-validate at the moment of writing rather than trusting the flag the
         * update loop left behind: a key edited on the same frame as START would
         * otherwise be exported against the previous frame's verdict. */
        forge_cine_validate(f);
        if (f->cine_err) {
            /* Refuse. A table with a hard failure in it is one the game will
             * reject or crash on — eye == look halts the VR4300 inside
             * t3d_viewport_attach, several layers from the table that caused it.
             * Writing it anyway moves that discovery two tools away from the
             * person holding the controller. */
            kiln_store_log("cam table NOT written: err %#lx at key %d",
                          (unsigned long)f->cine_err, f->cine_report.bad_key);
            cam_st = KILN_STORE_EVERSION;   /* "refused", surfaced on the HUD */
        } else {
            forge_cine_emit(f, g_aux, AUX_MAX_BYTES);
            cam_st = kiln_store_write_text(FORGE_LEVEL_NAME, "KEY", g_aux);
        }
    }
    if (lt_st != KILN_STORE_OK)
        kiln_store_log("light header not written: %s",
                      kiln_store_status_name(lt_st));
    if (cam_st != KILN_STORE_OK)
        f->last_store = cam_st, f->last_action = "cam";
    /* The .FRG is the save; the .MAP is a bonus that only the SD backend can
     * carry. Reporting the text failure would make an entirely successful save
     * look broken on the save-chip fallback, so it is logged and the .FRG's
     * status stands. */
    if (text_st != KILN_STORE_OK)
        kiln_store_log("map text not written: %s", kiln_store_status_name(text_st));
    else
        kiln_store_log("saved %s: %d brushes, %lu solid blocks",
                      FORGE_LEVEL_NAME, nb,
                      (unsigned long)kiln_voxel_solid_count(&f->world));
    return KILN_STORE_OK;
}

int forge_io_load(Forge *f)
{
    uint32_t len = 0;
    f->last_action = "load";
    f->last_store = kiln_store_read(FORGE_LEVEL_NAME, FRG_VERSION,
                                   g_frg, FRG_MAX_BYTES, &len);
    if (f->last_store != KILN_STORE_OK) return f->last_store;

    kiln_voxel_clear(&f->world);
    uint32_t at = 0;
    at = get_f32(g_frg, at, &f->world.offset.v[0]);
    at = get_f32(g_frg, at, &f->world.offset.v[1]);
    at = get_f32(g_frg, at, &f->world.offset.v[2]);
    uint16_t nchunks = 0;
    at = get_u16(g_frg, at, &nchunks);

    int refused = 0;
    static uint8_t blocks[KILN_VOXEL_CHUNK_BLOCKS];
    for (uint16_t i = 0; i < nchunks && at + 3 < len; i++) {
        int cx = g_frg[at++], cy = g_frg[at++], cz = g_frg[at++];
        memset(blocks, 0, sizeof blocks);
        at = rle_decode(g_frg, at, len, blocks, KILN_VOXEL_CHUNK_BLOCKS);

        /* Replay through kiln_voxel_set rather than memcpy'ing into a chunk:
         * the setter is what allocates the slot, keeps `solid` right and marks
         * neighbours dirty. Writing the array directly would load a level whose
         * chunk counts and mesh state are quietly wrong. */
        for (int z = 0; z < KILN_VOXEL_CHUNK; z++)
        for (int y = 0; y < KILN_VOXEL_CHUNK; y++)
        for (int x = 0; x < KILN_VOXEL_CHUNK; x++) {
            uint8_t b = blocks[(z * KILN_VOXEL_CHUNK + y) * KILN_VOXEL_CHUNK + x];
            if (b == KILN_VOXEL_AIR) continue;
            /* kiln_voxel_set refuses a block once KILN_VOXEL_MAX_CHUNKS is
             * reached. Discarding that return is how a level pushed from the
             * host arrives on the console missing whole rooms, with nothing to
             * say so but the `chunks n/24` gauge going red -- which nobody is
             * watching during a load. The host side now refuses to WRITE such
             * a file (tools/forge/frg.py's encode); this is the other end. */
            if (kiln_voxel_set(&f->world, cx * KILN_VOXEL_CHUNK + x,
                                          cy * KILN_VOXEL_CHUNK + y,
                                          cz * KILN_VOXEL_CHUNK + z, b) != 0)
                refused++;
        }
    }

    if (refused)
        debugf("forge_io: %d block(s) refused — the file needs more than "
               "KILN_VOXEL_MAX_CHUNKS (%d) chunks, so this level loaded "
               "INCOMPLETE\n", refused, KILN_VOXEL_MAX_CHUNKS);

    if (at + sizeof f->atlas.index <= len) {
        memcpy(f->atlas.index, &g_frg[at], sizeof f->atlas.index);
        at += sizeof f->atlas.index;
        for (int st = 0; st < 2; st++)
            for (int c = 0; c < KILN_VOXATLAS_COLOURS; c++) {
                uint16_t v; at = get_u16(g_frg, at, &v);
                f->atlas.tlut[st][c] = v;
            }
        f->atlas.dirty = 1;
    }

    /* Every section below is guarded on there being enough payload left, so a
     * .FRG written by an older ROM (or truncated) loads its geometry and leaves
     * the rest at defaults rather than reading past the buffer. The version check
     * in kiln_store_read already refuses a mismatch; this is the second line of
     * defence for a payload that is self-consistently short, which a checksum
     * cannot detect. */
    uint16_t nents = 0;
    if (at + 2 <= len) {
        at = get_u16(g_frg, at, &nents);
        if (nents > FORGE_MAX_ENTS) nents = FORGE_MAX_ENTS;
        f->ent_count = 0;
        for (uint16_t i = 0; i < nents && at + 21 <= len; i++) {
            ForgeEnt *e = &f->ents[f->ent_count];
            memset(e, 0, sizeof *e);
            at = get_f32(g_frg, at, &e->pos.v[0]);
            at = get_f32(g_frg, at, &e->pos.v[1]);
            at = get_f32(g_frg, at, &e->pos.v[2]);
            uint16_t a16; at = get_u16(g_frg, at, &a16);
            e->angle = (int16_t)a16;
            e->classname = g_frg[at++];
            if (e->classname >= FORGE_CLASSNAMES) e->classname = 0;
            for (int k = 0; k < FORGE_EPAIRS; k++) {
                uint16_t v; at = get_u16(g_frg, at, &v); e->epair[k] = v;
            }
            f->ent_count++;
        }
    }

    if (at + 25 <= len) {
        at = get_f32(g_frg, at, &f->key_yaw);
        at = get_f32(g_frg, at, &f->key_pitch);
        at = get_f32(g_frg, at, &f->fill_yaw);
        at = get_f32(g_frg, at, &f->fill_pitch);
        f->key_level = g_frg[at++];
        f->fill_level = g_frg[at++];
        f->ambient = g_frg[at++];
        f->fog_on = g_frg[at++];
        f->clear_idx = g_frg[at++];
        at = get_f32(g_frg, at, &f->fog_near);
        at = get_f32(g_frg, at, &f->fog_far);
        forge_light_apply(f);
    }

    if (at + 7 <= len) {
        uint16_t nk = 0;
        at = get_u16(g_frg, at, &nk);
        if (nk > FORGE_MAX_KEYS) nk = FORGE_MAX_KEYS;
        at = get_f32(g_frg, at, &f->cine_duration);
        f->cine_loop = g_frg[at++];
        f->key_count = 0;
        for (uint16_t i = 0; i < nk && at + 28 <= len; i++) {
            KilnCamKey *k = &f->keys[f->key_count];
            at = get_f32(g_frg, at, &k->t);
            for (int a = 0; a < 3; a++) at = get_f32(g_frg, at, &k->eye.v[a]);
            for (int a = 0; a < 3; a++) at = get_f32(g_frg, at, &k->look.v[a]);
            f->key_count++;
        }
        f->key_sel = f->key_count ? 0 : -1;
        forge_cine_validate(f);
    }

    forge_geo_remesh(f);
    return KILN_STORE_OK;
}

/* A starter room, so a first boot is not an empty void with a reticle in it. A
 * floor, three walls and a doorway — enough to make WALK mode meaningful
 * immediately, which is the mode that justifies the tool. */
void forge_io_seed(Forge *f)
{
    kiln_voxel_clear(&f->world);
    kiln_voxel_fill(&f->world, 0, 0, 0, 15, 0, 15, 1);        /* floor       */
    kiln_voxel_fill(&f->world, 0, 1, 0, 15, 4, 0, 2);         /* back wall   */
    kiln_voxel_fill(&f->world, 0, 1, 0, 0, 4, 15, 2);         /* left wall   */
    kiln_voxel_fill(&f->world, 15, 1, 0, 15, 4, 15, 2);       /* right wall  */
    kiln_voxel_fill(&f->world, 6, 1, 15, 9, 3, 15, KILN_VOXEL_AIR); /* doorway */
    kiln_voxel_fill(&f->world, 0, 1, 15, 5, 4, 15, 3);
    kiln_voxel_fill(&f->world, 10, 1, 15, 15, 4, 15, 3);

    /* A spawn and a three-key shot, so a FIRST BOOT exercises every mode rather
     * than only the two that need no content. An ENT panel over an empty list and
     * a CAM timeline with no keys both look like broken modes, and "is this mode
     * finished?" is not a question a tool should make its user ask. It is also
     * what the mode-jump capture ROMs need in order to show anything.
     *
     * Three keys, not two: Catmull-Rom takes its tangent from a key's two
     * NEIGHBOURS, so two keys give a straight line and demonstrate nothing about
     * the curve — which is the thing CAM mode is for looking at. */
    const float B = (float)KILN_VOXEL_BLOCK_UNITS;
    f->ent_count = 0;
    f->ents[0] = (ForgeEnt){
        .pos = {{ 8.5f * B, 1.5f * B, 8.5f * B }},
        .angle = 0,
        .classname = 0,          /* info_player_start */
    };
    f->ent_count = 1;
    f->ent_sel = 0;

    f->key_count = 3;
    f->cine_duration = 8.0f;
    f->keys[0] = (KilnCamKey){ 0.0f, {{ -2.0f * B, 6.0f * B, -2.0f * B }},
                                     {{  8.0f * B, 2.0f * B,  8.0f * B }} };
    f->keys[1] = (KilnCamKey){ 4.0f, {{  8.0f * B, 7.0f * B, -4.0f * B }},
                                     {{  8.0f * B, 2.0f * B,  8.0f * B }} };
    f->keys[2] = (KilnCamKey){ 8.0f, {{ 18.0f * B, 6.0f * B,  8.0f * B }},
                                     {{  8.0f * B, 2.0f * B,  8.0f * B }} };
    f->key_sel = 0;
    forge_cine_validate(f);

    forge_geo_remesh(f);
}
