/* SPDX-License-Identifier: MIT
 *
 * kiln_map.c — see kiln_map.h for the model.
 */

#include "kiln_map.h"

#include <libdragon.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <malloc.h>
#include <float.h>

/* The capacities come from kiln_levelvocab.h, which is generated from
 * tools/schema/level_vocab.json — the same numbers tools/mapmaker/validate.py
 * reports a level against. They used to be typed here and typed again there,
 * and nothing compared them. The short local names are kept so the rest of
 * this file is untouched. */
#include "kiln_levelvocab.h"

#define MAX_CLASSNAMES KILN_LEVEL_MAX_CLASSNAMES
#define MAX_BRUSHES    KILN_LEVEL_MAX_BRUSHES
#define MAX_FACES      KILN_LEVEL_MAX_FACES
#define MAX_SPAWNS     KILN_LEVEL_MAX_SPAWNS
#define MAX_ENTITIES   KILN_LEVEL_MAX_ENTITIES

typedef struct {
    const char *name;
    uint16_t    profile_id;
} ClassnameReg;

static ClassnameReg g_classes[MAX_CLASSNAMES];
static int          g_class_count;

/* Brushes discarded because MAX_BRUSHES was reached. Reset by kiln_map_load,
 * reported by it. kiln_map_load is not reentrant, so a file-static is the
 * whole mechanism. */
static int          g_dropped_brushes;

void kiln_map_register_classname(const char *classname, uint16_t profile_id)
{
    if (g_class_count >= MAX_CLASSNAMES) {
        debugf("kiln_map: classname table full, ignoring '%s'\n", classname);
        return;
    }
    size_t n = strlen(classname);
    char *copy = malloc(n + 1);
    memcpy(copy, classname, n + 1);
    g_classes[g_class_count].name = copy;
    g_classes[g_class_count].profile_id = profile_id;
    g_class_count++;
}

static uint16_t profile_for_classname(const char *classname)
{
    for (int i = 0; i < g_class_count; i++) {
        if (strcmp(g_classes[i].name, classname) == 0)
            return g_classes[i].profile_id;
    }
    return 0xFFFFu;
}

/* 1-based line number of `at` within `buf`. Only ever called on a failure
 * path, so the linear scan costs nothing that matters — and a byte offset is
 * not something a person can act on without opening the file in an editor
 * that shows them. */
static int line_of(const char *buf, const char *at)
{
    int line = 1;
    for (const char *c = buf; c < at && *c; c++)
        if (*c == '\n') line++;
    return line;
}

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

// Skip whitespace and C++ line comments. .map files in this repo
// (and Quake .map files generally) start with a header preamble of
// `// SPDX-...` comments, and authors also leave inline notes between
// entities. The first non-comment, non-whitespace token has to be a
// `{` for the outer entity loop to recognise an entity; without this
// the parser sees `/` at offset 0, fails the `{` check, and exits
// with 0 brushes and 0 spawns — a degenerate success that the
// caller has no way to distinguish from a real empty map.
static void skip_ws_and_comments(const char **p)
{
    while (1) {
        skip_ws(p);
        if (**p != '/' || (*p)[1] != '/') return;
        while (**p && **p != '\n') (*p)++;
    }
}

static int read_token(const char **p, char *out, size_t out_sz)
{
    skip_ws(p);
    if (**p == '\0') return 0;
    size_t n = 0;
    if (**p == '"') {
        (*p)++;
        while (**p != '"' && **p != '\0' && n + 1 < out_sz) {
            out[n++] = **p;
            (*p)++;
        }
        if (**p == '"') (*p)++;
    } else if (**p == '{' || **p == '}' || **p == '(' || **p == ')') {
        out[n++] = **p;
        (*p)++;
    } else {
        while (**p != '\0' && !isspace((unsigned char)**p)
               && **p != '{' && **p != '}' && **p != '(' && **p != ')'
               && n + 1 < out_sz) {
            out[n++] = **p;
            (*p)++;
        }
    }
    out[n] = '\0';
    return n > 0 ? 1 : 0;
}

static int expect_token(const char **p, const char *tok)
{
    char tmp[64];
    if (!read_token(p, tmp, sizeof(tmp))) return 0;
    return strcmp(tmp, tok) == 0;
}

static void update_aabb(fm_vec3_t *minv, fm_vec3_t *maxv, fm_vec3_t pt)
{
    for (int i = 0; i < 3; i++) {
        if (pt.v[i] < minv->v[i]) minv->v[i] = pt.v[i];
        if (pt.v[i] > maxv->v[i]) maxv->v[i] = pt.v[i];
    }
}

static int parse_brush(const char **p, KilnBrush *brush, KilnMapFace *faces,
                       int *face_idx, int max_faces)
{
    if (!expect_token(p, "{")) return -1;

    fm_vec3_t mins = {{  FLT_MAX,  FLT_MAX,  FLT_MAX }};
    fm_vec3_t maxs = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};

    while (1) {
        skip_ws(p);
        if (**p == '}') { (*p)++; break; }

        /* Each face is 3 points (each "( x y z )") followed by a texture name
         * and 5 UV/rotation/scale tokens. No outer parens wrap the face. */
        fm_vec3_t pts[3];
        for (int i = 0; i < 3; i++) {
            if (!expect_token(p, "(")) return -1;
            char tok[64];
            float v[3];
            for (int j = 0; j < 3; j++) {
                if (!read_token(p, tok, sizeof(tok))) return -1;
                v[j] = (float)atof(tok);
            }
            if (!expect_token(p, ")")) return -1;
            pts[i].v[0] = v[0];
            pts[i].v[1] = v[1];
            pts[i].v[2] = v[2];
            update_aabb(&mins, &maxs, pts[i]);
        }

        /* Texture name + 5 params (offset_x, offset_y, rotation, x_scale,
         * y_scale). Fixed-count read matches the Quake format exactly;
         * a malformed face fails cleanly rather than silently misaligning. */
        char tok[64];
        for (int i = 0; i < 6; i++) {
            if (!read_token(p, tok, sizeof(tok))) return -1;
        }

        if (*face_idx >= max_faces) {
            debugf("kiln_map: MAX_FACES (%d) reached; the load is abandoned\n",
                   max_faces);
            return -1;
        }

        /* Build a parallelogram face: p0, p1, p2, p0+p2-p1. */
        fm_vec3_t n;
        fm_vec3_t e1, e2;
        fm_vec3_sub(&e1, &pts[1], &pts[0]);
        fm_vec3_sub(&e2, &pts[2], &pts[0]);
        fm_vec3_cross(&n, &e1, &e2);
        if (fm_vec3_len2(&n) > 1e-12f) fm_vec3_norm(&n, &n);
        else n = (fm_vec3_t){{ 0, 1, 0 }};
        uint8_t np = t3d_vert_pack_normal(&n);

        fm_vec3_t p3;
        fm_vec3_sub(&p3, &pts[2], &pts[1]);
        fm_vec3_add(&p3, &pts[0], &p3);

        /* Pack 8 verts into 4 T3DVertPacked structs. */
        T3DVertPacked *v = malloc_uncached(sizeof(T3DVertPacked) * 4);
        if (!v) return -1;

        fm_vec3_t verts[8] = { pts[0], pts[1], pts[2], p3, pts[0], pts[1], pts[2], p3 };
        for (int i = 0; i < 4; i++) {
            v[i] = (T3DVertPacked){
                .posA = { (int16_t)verts[i*2+0].v[0], (int16_t)verts[i*2+0].v[1], (int16_t)verts[i*2+0].v[2] },
                .rgbaA = 0xFFFFFFFF,
                .normA = np,
                .posB = { (int16_t)verts[i*2+1].v[0], (int16_t)verts[i*2+1].v[1], (int16_t)verts[i*2+1].v[2] },
                .rgbaB = 0xFFFFFFFF,
                .normB = np,
            };
        }

        faces[*face_idx].verts = v;
        faces[*face_idx].rgba = 0xFFFFFFFF;
        (*face_idx)++;
    }

    brush->mins = mins;
    brush->maxs = maxs;
    brush->surface = 0;
    brush->flags = 0;
    return 0;
}

static int parse_entity(const char **p, KilnDict *epairs, KilnBrush *brushes,
                        int *brush_idx, int max_brushes,
                        KilnMapFace *faces, int *face_idx, int max_faces,
                        KilnRoomSpawn *spawn_out, int *has_spawn)
{
    if (!expect_token(p, "{")) return -1;

    kiln_dict_init(epairs);
    *has_spawn = 0;

    while (1) {
        skip_ws_and_comments(p);
        if (**p == '}') { (*p)++; break; }

        if (**p == '{') {
            KilnBrush b;
            if (parse_brush(p, &b, faces, face_idx, max_faces) < 0) return -1;
            if (brushes && *brush_idx < max_brushes) {
                brushes[*brush_idx] = b;
                (*brush_idx)++;
            } else if (brushes) {
                /* Same silent drop the spawn table had, but this one loses
                 * GEOMETRY: the brush is gone from the render AND from the
                 * clip world, so the wall is invisible and you walk through
                 * it. Counted rather than logged per brush, and reported once
                 * by kiln_map_load -- and NOT by advancing *brush_idx, which
                 * would push brush_count past the array the world-AABB loop
                 * then walks. */
                g_dropped_brushes++;
            }
            continue;
        }

        char key[64], val[256];
        if (!read_token(p, key, sizeof(key))) return -1;
        if (!read_token(p, val, sizeof(val))) return -1;
        kiln_dict_set_auto(epairs, key, val);
    }

    const char *cn = kiln_dict_get_str(epairs, "classname", NULL);
    if (!cn) return 0; /* malformed entity, but not fatal */

    uint16_t pid = profile_for_classname(cn);
    if (pid != 0xFFFFu) {
        fm_vec3_t origin = kiln_dict_get_vec3(epairs, "origin", (fm_vec3_t){{0,0,0}});
        float yaw = (float)kiln_dict_get_int(epairs, "angle", 0);
        spawn_out->profile_id = pid;
        spawn_out->pos = origin;
        spawn_out->yaw = yaw * (M_PI / 180.0f);
        /* Copy the spawn args into the spawn template. */
        spawn_out->dict = *epairs;
        *has_spawn = 1;
    }

    return 0;
}

int kiln_map_load(KilnMap *out, const char *dfs_path)
{
    memset(out, 0, sizeof(*out));

    // libdragon's dfs_open takes a native DFS path ("maps/foo.map"), not
    // the newlib-style "rom:/maps/foo.map" prefix used by fopen et al.
    // Callers in this repo were passing the newlib form by convention and
    // seeing spurious ENOFILE — strip the prefix here so both forms work.
    const char *native_path = dfs_path;
    if (native_path && strncmp(native_path, "rom:/", 5) == 0) {
        native_path += 5;
    }

    int fh = dfs_open(native_path);
    if (fh < 0) {
        debugf("kiln_map: dfs_open(%s) failed\n", dfs_path);
        return -1;
    }
    uint32_t sz = dfs_size(fh);
    char *buf = malloc(sz + 1);
    if (!buf) {
        dfs_close(fh);
        return -1;
    }
    dfs_read(buf, 1, sz, fh);
    dfs_close(fh);
    buf[sz] = '\0';

    KilnBrush  *brushes  = malloc(sizeof(KilnBrush)  * MAX_BRUSHES);
    KilnMapFace *faces    = malloc(sizeof(KilnMapFace) * MAX_FACES);
    KilnRoomSpawn *spawns = malloc(sizeof(KilnRoomSpawn) * MAX_SPAWNS);
    if (!brushes || !faces || !spawns) {
        free(buf); free(brushes); free(faces); free(spawns);
        return -1;
    }

    const char *p = buf;
    int brush_count = 0;
    int face_count = 0;
    int spawn_count = 0;

    g_dropped_brushes = 0;

    fm_vec3_t world_min = {{ FLT_MAX, FLT_MAX, FLT_MAX }};
    fm_vec3_t world_max = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};

    while (1) {
        skip_ws_and_comments(&p);
        if (*p == '\0') break;
        if (*p != '{') {
            debugf("kiln_map: %s:%d: expected '{', got '%c'\n",
                   dfs_path, line_of(buf, p), *p);
            break;
        }

        const char *entity_start = p;
        int brush_count_before = brush_count;
        KilnDict epairs;
        KilnRoomSpawn spawn;
        int has_spawn = 0;
        if (parse_entity(&p, &epairs, brushes, &brush_count, MAX_BRUSHES,
                         faces, &face_count, MAX_FACES,
                         &spawn, &has_spawn) < 0) {
            debugf("kiln_map: %s:%d: failed to parse entity\n",
                   dfs_path, line_of(buf, entity_start));
            break;
        }

        /* Update the world AABB from any brushes added by this entity. */
        for (int i = brush_count_before; i < brush_count; i++) {
            update_aabb(&world_min, &world_max, brushes[i].mins);
            update_aabb(&world_min, &world_max, brushes[i].maxs);
        }

        const char *cn = kiln_dict_get_str(&epairs, "classname", "");
        if (strcmp(cn, "worldspawn") != 0 && has_spawn) {
            if (spawn_count < MAX_SPAWNS) {
                spawns[spawn_count++] = spawn;
            } else {
                /* Dropping this silently is how a level loses its 65th entity
                 * and nobody finds out until someone notices a door that never
                 * opens. Every other capacity in this file reports; this one
                 * did not. */
                debugf("kiln_map: %s: MAX_SPAWNS (%d) reached, dropping '%s'\n",
                       dfs_path, MAX_SPAWNS, cn);
            }
        }
    }

    if (g_dropped_brushes)
        debugf("kiln_map: %s: MAX_BRUSHES (%d) reached; %d brush(es) dropped "
               "from both the mesh and the clip world\n",
               dfs_path, MAX_BRUSHES, g_dropped_brushes);

    free(buf);

    out->brushes     = brushes;
    out->brush_count = brush_count;
    out->faces       = faces;
    out->face_count  = face_count;
    out->spawns      = spawns;
    out->spawn_count = spawn_count;
    out->world_aabb_min = world_min;
    out->world_aabb_max = world_max;
    return 0;
}

void kiln_map_free(KilnMap *m)
{
    if (!m) return;
    for (int i = 0; i < m->face_count; i++) {
        if (m->faces[i].verts) free_uncached(m->faces[i].verts);
    }
    free(m->faces);
    free(m->brushes);
    free(m->spawns);
    m->brushes = NULL;
    m->faces = NULL;
    m->spawns = NULL;
}

void kiln_map_draw(const KilnMap *m)
{
    if (!m) return;
    for (int i = 0; i < m->face_count; i++) {
        t3d_vert_load(m->faces[i].verts, 0, 8);
        /* Parallelogram: (0,1,2) and (0,2,3) using the packed vertex indices.
         * Vertex order in each T3DVertPacked is A then B; with 4 structs we
         * have verts 0..7 in that order. */
        t3d_tri_draw(0, 1, 2);
        t3d_tri_draw(0, 2, 3);
        t3d_tri_sync();
    }
}