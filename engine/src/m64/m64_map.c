/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_map.c — see m64_map.h for the model.
 */

#include "m64_map.h"

#include <libdragon.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <malloc.h>
#include <float.h>

#define MAX_CLASSNAMES 32
#define MAX_BRUSHES    256
#define MAX_FACES      (MAX_BRUSHES * 6)
#define MAX_SPAWNS     64
#define MAX_ENTITIES   64

typedef struct {
    const char *name;
    uint16_t    profile_id;
} ClassnameReg;

static ClassnameReg g_classes[MAX_CLASSNAMES];
static int          g_class_count;

void m64_map_register_classname(const char *classname, uint16_t profile_id)
{
    if (g_class_count >= MAX_CLASSNAMES) {
        debugf("m64_map: classname table full, ignoring '%s'\n", classname);
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

static void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
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

static int parse_brush(const char **p, M64Brush *brush, M64MapFace *faces,
                       int *face_idx, int max_faces)
{
    if (!expect_token(p, "{")) return -1;

    fm_vec3_t mins = {{  FLT_MAX,  FLT_MAX,  FLT_MAX }};
    fm_vec3_t maxs = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};

    while (1) {
        skip_ws(p);
        if (**p == '}') { (*p)++; break; }

        if (!expect_token(p, "(")) return -1;

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

        /* Skip the texture/UV/scale tokens (up to 5) until the next
         * '(' starts the next face or '}' ends the brush. */
        char tok[64];
        while (1) {
            skip_ws(p);
            if (**p == '(' || **p == '}') break;
            read_token(p, tok, sizeof(tok));
        }

        if (*face_idx >= max_faces) return -1;

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
            fm_vec3_t na = verts[i * 2 + 0];
            fm_vec3_t nb = verts[i * 2 + 1];
            fm_vec3_norm(&na, &na);
            fm_vec3_norm(&nb, &nb);
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

static int parse_entity(const char **p, M64Dict *epairs, M64Brush *brushes,
                        M64MapFace *faces, int *face_idx, int max_faces,
                        M64RoomSpawn *spawn_out, int *has_spawn)
{
    if (!expect_token(p, "{")) return -1;

    m64_dict_init(epairs);
    *has_spawn = 0;

    while (1) {
        skip_ws(p);
        if (**p == '}') { (*p)++; break; }

        if (**p == '{') {
            M64Brush b;
            if (parse_brush(p, &b, faces, face_idx, max_faces) < 0) return -1;
            if (brushes) {
                /* Caller passes NULL for entity brushes if they only want
                 * worldspawn geometry; but we always parse them to skip past. */
            }
            continue;
        }

        char key[64], val[256];
        if (!read_token(p, key, sizeof(key))) return -1;
        if (!read_token(p, val, sizeof(val))) return -1;
        m64_dict_set_auto(epairs, key, val);
    }

    const char *cn = m64_dict_get_str(epairs, "classname", NULL);
    if (!cn) return 0; /* malformed entity, but not fatal */

    uint16_t pid = profile_for_classname(cn);
    if (pid != 0xFFFFu) {
        fm_vec3_t origin = m64_dict_get_vec3(epairs, "origin", (fm_vec3_t){{0,0,0}});
        float yaw = (float)m64_dict_get_int(epairs, "angle", 0);
        spawn_out->profile_id = pid;
        spawn_out->pos = origin;
        spawn_out->yaw = yaw * (M_PI / 180.0f);
        /* Copy the spawn args into the spawn template. */
        spawn_out->dict = *epairs;
        *has_spawn = 1;
    }

    return 0;
}

int m64_map_load(M64Map *out, const char *dfs_path)
{
    memset(out, 0, sizeof(*out));

    int fh = dfs_open(dfs_path);
    if (fh < 0) {
        debugf("m64_map: dfs_open(%s) failed\n", dfs_path);
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

    M64Brush  *brushes  = malloc(sizeof(M64Brush)  * MAX_BRUSHES);
    M64MapFace *faces    = malloc(sizeof(M64MapFace) * MAX_FACES);
    M64RoomSpawn *spawns = malloc(sizeof(M64RoomSpawn) * MAX_SPAWNS);
    if (!brushes || !faces || !spawns) {
        free(buf); free(brushes); free(faces); free(spawns);
        return -1;
    }

    const char *p = buf;
    int brush_count = 0;
    int face_count = 0;
    int spawn_count = 0;

    fm_vec3_t world_min = {{ FLT_MAX, FLT_MAX, FLT_MAX }};
    fm_vec3_t world_max = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};

    while (1) {
        skip_ws(&p);
        if (*p == '\0') break;
        if (*p != '{') {
            debugf("m64_map: expected '{' at offset %zu, got '%c'\n", (size_t)(p - buf), *p);
            break;
        }

        M64Dict epairs;
        M64RoomSpawn spawn;
        int has_spawn = 0;
        if (parse_entity(&p, &epairs, brushes, faces, &face_count, MAX_FACES,
                         &spawn, &has_spawn) < 0) {
            debugf("m64_map: failed to parse entity\n");
            break;
        }

        const char *cn = m64_dict_get_str(&epairs, "classname", "");
        if (strcmp(cn, "worldspawn") == 0) {
            /* Worldspawn: copy its brushes from the face-building path. We
             * need to replay the entity to capture brush AABBs. That's
             * awkward with the current single-pass design, so instead we
             * special-case: worldspawn entities' brushes were already
             * parsed into `faces`; we reconstruct their AABBs by walking
             * the face vertex positions. For the demo this is fine because
             * worldspawn is exactly one entity. */
            /* In this simplified parser, worldspawn brushes got faces but
             * not AABBs stored. We skip worldspawn spawn generation and rely
             * on the caller to draw faces directly; collision needs brushes,
             * which we will back-fill from faces below if no brushes were
             * captured. */
            for (int i = 0; i < face_count; i++) {
                /* Recompute AABB from the four packed verts. */
                fm_vec3_t bmin = {{ FLT_MAX, FLT_MAX, FLT_MAX }};
                fm_vec3_t bmax = {{ -FLT_MAX, -FLT_MAX, -FLT_MAX }};
                for (int j = 0; j < 4; j++) {
                    fm_vec3_t pa = {{ (float)faces[i].verts[j].posA[0],
                                      (float)faces[i].verts[j].posA[1],
                                      (float)faces[i].verts[j].posA[2] }};
                    fm_vec3_t pb = {{ (float)faces[i].verts[j].posB[0],
                                      (float)faces[i].verts[j].posB[1],
                                      (float)faces[i].verts[j].posB[2] }};
                    update_aabb(&bmin, &bmax, pa);
                    update_aabb(&bmin, &bmax, pb);
                }
                if (brush_count < MAX_BRUSHES) {
                    brushes[brush_count].mins = bmin;
                    brushes[brush_count].maxs = bmax;
                    brushes[brush_count].surface = 0;
                    brushes[brush_count].flags = 0;
                    brush_count++;
                }
                update_aabb(&world_min, &world_max, bmin);
                update_aabb(&world_min, &world_max, bmax);
            }
        } else if (has_spawn) {
            if (spawn_count < MAX_SPAWNS) {
                spawns[spawn_count++] = spawn;
            }
        }
    }

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

void m64_map_free(M64Map *m)
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

void m64_map_draw(const M64Map *m)
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