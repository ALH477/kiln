/* SPDX-License-Identifier: MIT
 *
 * host_t3dmodel.c — a .t3dm reader, and the drawing path over it.
 *
 * This is the first piece of the host backend that reimplements a FILE FORMAT
 * rather than an API, and the reason is unavoidable. Tiny3D's loader is an
 * in-place relocation: the file is read into memory, its structs ARE the file
 * layout, and "pointers" stored as offsets get a base added. That works on a
 * 32-bit target where a pointer is four bytes, which is exactly what the file
 * reserves. On x86-64 it is eight, so every field after the first pointer
 * would be read from the wrong offset. Casting the buffer is not available;
 * parsing it is.
 *
 * ── The format, read off Tiny3D's own sources and confirmed against bytes ──
 * Big-endian throughout. Header:
 *
 *   0x00  char     magic[3] = "T3M", then a version byte (4)
 *   0x04  uint32   chunkCount
 *   0x08  uint16   totalVertCount
 *   0x0A  uint16   totalIndexCount
 *   0x0C  uint32   chunkIdxVertices     index INTO the chunk table
 *   0x10  uint32   chunkIdxIndices
 *   0x14  uint32   chunkIdxMaterials
 *   0x18  uint32   stringTablePtr       file offset
 *   0x1C  uint32   userBlock            (runtime only)
 *   0x20  int16    aabbMin[3]
 *   0x26  int16    aabbMax[3]
 *   0x2C  uint32   chunkOffsets[chunkCount]
 *
 * Each chunk-table word packs the TYPE into its top byte and the file offset
 * into the low 24 bits — a union{char type; uint32 offset} upstream, which
 * only works because the target is big-endian. Types: 'V' vertices,
 * 'I' indices, 'M' material, 'O' object, 'S' skeleton, 'A' anim, 'B' BVH.
 *
 * An object chunk is 32 bytes then numParts x 24-byte parts:
 *
 *   obj  +0x00 uint32 name (offset into the string table, 0 = none)
 *        +0x04 uint16 numParts        +0x06 uint16 triCount
 *        +0x08 uint32 material        RELATIVE to chunkIdxMaterials
 *        +0x0C uint32 userBlock
 *        +0x10 uint8  isVisible, _pad, userValue0, userValue1
 *        +0x14 int16  aabbMin[3]      +0x1A int16 aabbMax[3]
 *   part +0x00 uint32 vert            offset into the VERTICES chunk
 *        +0x04 uint16 vertLoadCount   +0x06 uint16 vertDestOffset
 *        +0x08 uint32 indices         offset into the INDICES chunk
 *        +0x0C uint16 numIndices      +0x0E uint16 matrixIdx
 *        +0x10 uint8  numStripIndices[4]
 *        +0x14 uint8  idxSeqBase      +0x15 uint8 idxSeqCount
 *
 * ── Strips are the normal case, not the exotic one ────────────────────
 * gltf_to_t3d emits triangle STRIPS: the very first model checked here — a
 * cube — has numIndices 0 and numStripIndices[0] = 24. So a reader that only
 * handled indexed triangles would load every model in this repo and draw
 * nothing, which is the politest possible failure and the worst.
 *
 * The strip buffers sit after the part's indices, each 8-byte aligned, as
 * int16 arrays. Tiny3D rewrites them in place into DMEM pointers
 * (t3d_indexbuffer_convert); the host keeps them raw and interprets them
 * directly, so the encoding is the documented one: first three values are a
 * triangle, each value after that extends the strip by one with the winding
 * flipped, and an index with bit 15 set restarts the strip.
 */
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3danim.h>
#include <libdragon.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T3DM_VERSION 4

static inline uint32_t rd32(const uint8_t *b, size_t o)
{
    return ((uint32_t)b[o] << 24) | ((uint32_t)b[o+1] << 16)
         | ((uint32_t)b[o+2] << 8) | b[o+3];
}
static inline uint16_t rd16(const uint8_t *b, size_t o)
{
    return (uint16_t)(((uint16_t)b[o] << 8) | b[o+1]);
}
static inline int16_t rds16(const uint8_t *b, size_t o)
{
    return (int16_t)rd16(b, o);
}

typedef struct {
    char     type;
    uint32_t off;
} Chunk;

/* T3DVertPacked, field by field: 8 u16 (two positions + two normals), then
 * 2 u32 (the colours), then 4 u16 (the UVs). Doing it by field rather than
 * blanket-swapping 16-bit words is what keeps rgba correct. */
static void swap_vert_struct(uint8_t *d, const uint8_t *s)
{
    for (int i = 0; i < 8; i++) {
        d[i*2 + 0] = s[i*2 + 1];
        d[i*2 + 1] = s[i*2 + 0];
    }
    for (int i = 0; i < 2; i++) {
        const size_t o = 16 + (size_t)i * 4;
        d[o+0] = s[o+3]; d[o+1] = s[o+2]; d[o+2] = s[o+1]; d[o+3] = s[o+0];
    }
    for (int i = 0; i < 4; i++) {
        const size_t o = 24 + (size_t)i * 2;
        d[o+0] = s[o+1]; d[o+1] = s[o+0];
    }
}

typedef struct {
    T3DModel     pub;
    uint8_t     *raw;
    T3DVertPacked *verts;      /* host-endian copy of the vertex chunk     */
    uint32_t     vertStructs;
    int16_t     *stripPool;    /* host-endian copies of every strip buffer */
    uint32_t     stripPoolLen;
    int          size;
    Chunk       *chunks;
    uint32_t     chunkCount;
    uint32_t     vbase, ibase, strtab;
    uint32_t     matChunkIdx;
    T3DObject   *objects;
    uint32_t     objectCount;
    T3DMaterial *materials;
    uint32_t     materialCount;
    T3DObjectPart *parts;      /* one flat pool for every object's parts */
} Model;

static Model *as_model(const T3DModel *m)
{
    /* T3DModel is the first member of Model, so the public pointer IS the
     * private one. Same trick libdragon uses for surface_t. */
    return (Model *)(void *)(uintptr_t)m;
}

static char *strtab_at(Model *M, uint32_t off)
{
    if (off == 0) return NULL;
    const uint32_t at = M->strtab + off;
    assertf(at < (uint32_t)M->size,
            "t3dm: string offset %u runs past the %d-byte file", at, M->size);
    return (char *)M->raw + at;
}

T3DModel *t3d_model_load_buf(void *buf, int size)
{
    assertf(buf != NULL, "t3d_model_load_buf: NULL buffer");
    assertf(size >= 0x2C, "t3d_model_load_buf: %d bytes is too small for a "
                          ".t3dm header", size);
    const uint8_t *b = buf;
    assertf(memcmp(b, "T3M", 3) == 0,
            "t3d_model_load_buf: bad magic %c%c%c, expected T3M",
            b[0], b[1], b[2]);
    assertf(b[3] == T3DM_VERSION,
            "t3d_model_load_buf: .t3dm version %d, this reader speaks %d. "
            "gltf_to_t3d changed the format; re-read Tiny3D's t3dmodel.c.",
            b[3], T3DM_VERSION);

    Model *M = calloc(1, sizeof *M);
    assertf(M != NULL, "t3d_model_load_buf: out of memory");
    M->raw = buf;
    M->size = size;
    memcpy(M->pub.magic, b, 4);
    M->chunkCount = rd32(b, 0x04);
    M->pub.totalVertCount  = rd16(b, 0x08);
    M->pub.totalIndexCount = rd16(b, 0x0A);
    const uint32_t idxV = rd32(b, 0x0C);
    const uint32_t idxI = rd32(b, 0x10);
    M->matChunkIdx = rd32(b, 0x14);
    M->strtab = rd32(b, 0x18);
    for (int i = 0; i < 3; i++) {
        M->pub.aabbMin[i] = rds16(b, 0x20 + 2u * (size_t)i);
        M->pub.aabbMax[i] = rds16(b, 0x26 + 2u * (size_t)i);
    }

    assertf(0x2C + 4u * (size_t)M->chunkCount <= (size_t)size,
            "t3dm: %u chunks do not fit in %d bytes", M->chunkCount, size);
    M->chunks = calloc(M->chunkCount ? M->chunkCount : 1, sizeof *M->chunks);
    assertf(M->chunks != NULL, "t3dm: out of memory");
    for (uint32_t i = 0; i < M->chunkCount; i++) {
        const uint32_t w = rd32(b, 0x2C + 4u * (size_t)i);
        M->chunks[i].type = (char)(w >> 24);
        M->chunks[i].off  = w & 0x00FFFFFF;
        assertf(M->chunks[i].off < (uint32_t)size,
                "t3dm: chunk %u ('%c') offset %#x is past the end of a %d-byte "
                "file", i, M->chunks[i].type, M->chunks[i].off, size);
    }
    assertf(idxV < M->chunkCount && idxI < M->chunkCount,
            "t3dm: vertex/index chunk indices %u/%u are out of range (%u chunks)",
            idxV, idxI, M->chunkCount);
    M->vbase = M->chunks[idxV].off;
    M->ibase = M->chunks[idxI].off;

    /* ── the vertex chunk, converted ──
     * Two vertices per 32-byte struct, so totalVertCount rounds up. */
    M->vertStructs = ((uint32_t)M->pub.totalVertCount + 1u) / 2u;
    if (M->vertStructs) {
        assertf(M->vbase + M->vertStructs * 32u <= (uint32_t)size,
                "t3dm: %u vertices from %#x run past the %d-byte file",
                M->pub.totalVertCount, M->vbase, size);
        M->verts = calloc(M->vertStructs, sizeof *M->verts);
        assertf(M->verts != NULL, "t3dm: out of memory");
        for (uint32_t i = 0; i < M->vertStructs; i++)
            swap_vert_struct((uint8_t *)&M->verts[i],
                             M->raw + M->vbase + i * 32u);
    }

    /* ── materials ── */
    for (uint32_t i = 0; i < M->chunkCount; i++)
        if (M->chunks[i].type == T3D_CHUNK_TYPE_MATERIAL) M->materialCount++;
    M->materials = calloc(M->materialCount ? M->materialCount : 1,
                          sizeof *M->materials);
    assertf(M->materials != NULL, "t3dm: out of memory");
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < M->chunkCount; i++) {
            if (M->chunks[i].type != T3D_CHUNK_TYPE_MATERIAL) continue;
            const uint32_t o = M->chunks[i].off;
            /* Only the fields anything here reads. The material chunk also
             * carries the combiner words, blend mode and two texture
             * descriptors; the host does not act on them yet, and inventing
             * behaviour from a field it does not honour would be worse than
             * leaving it. */
            M->materials[n].renderFlags = rd32(b, o + 0x1C);
            M->materials[n].fogMode = b[o + 0x21];
            n++;
        }
    }

    /* ── objects and their parts ── */
    for (uint32_t i = 0; i < M->chunkCount; i++)
        if (M->chunks[i].type == T3D_CHUNK_TYPE_OBJECT) M->objectCount++;
    M->objects = calloc(M->objectCount ? M->objectCount : 1, sizeof *M->objects);
    assertf(M->objects != NULL, "t3dm: out of memory");

    uint32_t totalParts = 0;
    for (uint32_t i = 0; i < M->chunkCount; i++)
        if (M->chunks[i].type == T3D_CHUNK_TYPE_OBJECT)
            totalParts += rd16(b, M->chunks[i].off + 0x04);
    M->parts = calloc(totalParts ? totalParts : 1, sizeof *M->parts);
    assertf(M->parts != NULL, "t3dm: out of memory");

    uint32_t oi = 0, pcursor = 0;
    for (uint32_t i = 0; i < M->chunkCount; i++) {
        if (M->chunks[i].type != T3D_CHUNK_TYPE_OBJECT) continue;
        const uint32_t o = M->chunks[i].off;
        T3DObject *obj = &M->objects[oi++];
        obj->name     = strtab_at(M, rd32(b, o + 0x00));
        obj->numParts = rd16(b, o + 0x04);
        obj->triCount = rd16(b, o + 0x06);
        obj->isVisible  = b[o + 0x10];
        obj->userValue0 = b[o + 0x12];
        obj->userValue1 = b[o + 0x13];
        for (int k = 0; k < 3; k++) {
            obj->aabbMin[k] = rds16(b, o + 0x14 + 2u * (size_t)k);
            obj->aabbMax[k] = rds16(b, o + 0x1A + 2u * (size_t)k);
        }
        /* The material field is an index RELATIVE to chunkIdxMaterials, not a
         * pointer and not an absolute chunk index. */
        {
            const uint32_t rel = rd32(b, o + 0x08);
            const uint32_t abs_ = M->matChunkIdx + rel;
            if (abs_ < M->chunkCount &&
                M->chunks[abs_].type == T3D_CHUNK_TYPE_MATERIAL) {
                uint32_t n = 0;
                for (uint32_t k = 0; k < abs_; k++)
                    if (M->chunks[k].type == T3D_CHUNK_TYPE_MATERIAL) n++;
                obj->material = &M->materials[n];
            }
        }

        obj->parts = &M->parts[pcursor];
        for (uint32_t p = 0; p < obj->numParts; p++) {
            const uint32_t po = o + 0x20 + 24u * (size_t)p;
            T3DObjectPart *part = &M->parts[pcursor++];
            const uint32_t vrel = rd32(b, po + 0x00);
            part->vertLoadCount  = rd16(b, po + 0x04);
            part->vertDestOffset = rd16(b, po + 0x06);
            const uint32_t irel  = rd32(b, po + 0x08);
            part->numIndices = rd16(b, po + 0x0C);
            part->matrixIdx  = rd16(b, po + 0x0E);
            memcpy(part->numStripIndices, &b[po + 0x10], 4);
            part->idxSeqBase  = b[po + 0x14];
            part->idxSeqCount = b[po + 0x15];

            assertf(vrel % 32u == 0,
                    "t3dm: object %u part %u vertex offset %#x is not a "
                    "multiple of 32; T3DVertPacked is 32 bytes and holds two "
                    "vertices", oi - 1, p, vrel);
            assertf(vrel / 32u < M->vertStructs || part->vertLoadCount == 0,
                    "t3dm: object %u part %u points at vertex struct %u of %u",
                    oi - 1, p, vrel / 32u, M->vertStructs);
            part->vert = M->verts ? &M->verts[vrel / 32u] : NULL;
            part->indices = (uint8_t *)(M->raw + M->ibase + irel);

            /* Strips follow the indices in the FILE, each run aligned to 8 on
             * the file offset — not on a host pointer, which is a different
             * thing once malloc's own alignment is in play. Converted into the
             * strip pool below rather than read in place. */
            uint32_t so = M->ibase + irel + part->numIndices;
            for (int s = 0; s < 4; s++) {
                if (part->numStripIndices[s] == 0) { part->strips[s] = NULL; continue; }
                so = (so + 7u) & ~7u;
                assertf(so + (uint32_t)part->numStripIndices[s] * 2u
                        <= (uint32_t)size,
                        "t3dm: object %u part %u strip %d runs past the end of "
                        "the file", oi - 1, p, s);
                part->strips[s] = (int16_t *)(uintptr_t)so;  /* offset for now */
                so += (uint32_t)part->numStripIndices[s] * 2u;
                M->stripPoolLen += part->numStripIndices[s];
            }
        }
    }

    /* Second pass for the strips: the pool size is only known once every part
     * has been walked, and a realloc per part would invalidate the pointers
     * already handed out. */
    if (M->stripPoolLen) {
        M->stripPool = calloc(M->stripPoolLen, sizeof *M->stripPool);
        assertf(M->stripPool != NULL, "t3dm: out of memory");
        uint32_t cur = 0;
        for (uint32_t pi = 0; pi < totalParts; pi++) {
            T3DObjectPart *part = &M->parts[pi];
            for (int s = 0; s < 4; s++) {
                if (!part->numStripIndices[s]) continue;
                const uint32_t off = (uint32_t)(uintptr_t)part->strips[s];
                int16_t *dst = &M->stripPool[cur];
                for (int k = 0; k < part->numStripIndices[s]; k++)
                    dst[k] = (int16_t)rd16(b, off + 2u * (size_t)k);
                part->strips[s] = dst;
                cur += part->numStripIndices[s];
            }
        }
    }

    M->pub._raw = M->raw;
    M->pub._chunkCount = M->chunkCount;
    M->pub._chunks = M->chunks;
    M->pub._objects = M->objects;
    M->pub._objectCount = M->objectCount;
    return &M->pub;
}

T3DModel *t3d_model_load(const char *path)
{
    assertf(path != NULL, "t3d_model_load: NULL path");
    /* The engine passes DFS paths ("rom:/models/x.t3dm"); the host VFS maps
     * those onto a directory. Until that lands, strip the prefix and read
     * relative — and say so if it fails, rather than returning NULL for a
     * caller that does not check. */
    const char *p = path;
    if (strncmp(p, "rom:/", 5) == 0) p += 5;
    FILE *f = fopen(p, "rb");
    assertf(f != NULL, "t3d_model_load: cannot open '%s' (from '%s')", p, path);
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    assertf(sz > 0, "t3d_model_load: '%s' is empty", p);
    void *buf = malloc((size_t)sz);
    assertf(buf != NULL, "t3d_model_load: out of memory");
    assertf(fread(buf, 1, (size_t)sz, f) == (size_t)sz,
            "t3d_model_load: short read on '%s'", p);
    fclose(f);
    return t3d_model_load_buf(buf, (int)sz);
}

void t3d_model_free(T3DModel *model)
{
    if (!model) return;
    Model *M = as_model(model);
    free(M->stripPool); free(M->verts);
    free(M->parts); free(M->objects); free(M->materials); free(M->chunks);
    free(M->raw);
    free(M);
}

/* ── accessors ────────────────────────────────────────────────────────── */

T3DVertPacked *t3d_model_get_vertices(const T3DModel *model)
{
    /* The host-endian copy, not the file bytes — a caller writing through this
     * (kiln_vanim's morph path) must see the same representation the drawing
     * path reads. */
    return as_model(model)->verts;
}

T3DObject *t3d_model_get_object_by_index(const T3DModel *model, uint32_t index)
{
    Model *M = as_model(model);
    assertf(index < M->objectCount, "t3d_model_get_object_by_index: %u of %u",
            index, M->objectCount);
    return &M->objects[index];
}

T3DObject *t3d_model_get_object(const T3DModel *model, const char *name)
{
    Model *M = as_model(model);
    assertf(name != NULL, "t3d_model_get_object: NULL name");
    for (uint32_t i = 0; i < M->objectCount; i++)
        if (M->objects[i].name && strcmp(M->objects[i].name, name) == 0)
            return &M->objects[i];
    return NULL;
}

void t3d_model_make_object_vert_placeholder(const T3DModel *model,
                                            T3DObject *object,
                                            uint8_t segmentId)
{
    (void)model; (void)segmentId;
    T3DObject *obj = object;
    /* Upstream swaps the object's vertex pointer for a placeholder so a caller
     * can stream vertices in (kiln_vanim's morph path). Nothing in the host
     * drawing path acts on it yet, and quietly doing nothing would make a
     * morph render its base pose — which looks like the animation not
     * playing. Refuse instead. */
    (void)obj;
    assertf(0, "t3d_model_make_object_vert_placeholder is not implemented on "
               "the host: kiln_vanim's morph path has no host equivalent yet.");
}

T3DModelIter t3d_model_iter_create(const T3DModel *model, enum T3DChunkType type)
{
    T3DModelIter it;
    memset(&it, 0, sizeof it);
    it._model = model;
    it._idx = 0;
    it._chunkType = (char)type;
    return it;
}

bool t3d_model_iter_next(T3DModelIter *iter)
{
    assertf(iter != NULL && iter->_model != NULL, "t3d_model_iter_next: NULL");
    Model *M = as_model(iter->_model);
    while (iter->_idx < M->chunkCount) {
        const uint32_t i = iter->_idx++;
        if (M->chunks[i].type != iter->_chunkType) continue;
        if (iter->_chunkType == T3D_CHUNK_TYPE_OBJECT) {
            uint32_t n = 0;
            for (uint32_t k = 0; k < i; k++)
                if (M->chunks[k].type == T3D_CHUNK_TYPE_OBJECT) n++;
            iter->object = &M->objects[n];
        } else if (iter->_chunkType == T3D_CHUNK_TYPE_MATERIAL) {
            uint32_t n = 0;
            for (uint32_t k = 0; k < i; k++)
                if (M->chunks[k].type == T3D_CHUNK_TYPE_MATERIAL) n++;
            iter->material = &M->materials[n];
        } else {
            iter->chunk = M->raw + M->chunks[i].off;
        }
        return true;
    }
    return false;
}

/* ── drawing ──────────────────────────────────────────────────────────── */

void t3d_model_draw_material(T3DMaterial *mat, void *state)
{
    (void)state;
    /* The material chunk's combiner words are parsed but not applied: the host
     * combiner is a small set of named sentinels rather than a real RDP
     * register encoding (see plat/host/include/libdragon.h), so there is
     * nothing honest to map an arbitrary combiner onto. A model therefore
     * draws under whatever combiner the caller last set — which is exactly
     * what kiln_scene_begin's RDPQ_COMBINER_SHADE means for an untextured
     * model, and is why nix/checks/kiln-voxmesh.nix exists. */
    (void)mat;
}

void t3d_model_draw_object(const T3DObject *object, const T3DMat4FP *boneMatrices)
{
    assertf(object != NULL, "t3d_model_draw_object: NULL object");
    if (boneMatrices) {
        /* Skinning needs a matrix per bone and t3dskeleton is not implemented.
         * Refusing beats drawing the base pose, which reads as "the animation
         * is not playing" rather than "skinning is missing". */
        assertf(0, "t3d_model_draw_object with bone matrices: skinning is not "
                   "implemented in the host 3D pass.");
    }
    for (uint32_t p = 0; p < object->numParts; p++) {
        const T3DObjectPart *part = &object->parts[p];
        t3d_vert_load(part->vert, part->vertDestOffset, part->vertLoadCount);

        if (part->numIndices == 0 && part->numStripIndices[0] == 0 &&
            part->idxSeqCount == 0)
            continue;   /* partial load: a later part carries the indices */

        for (uint16_t i = 0; i + 2 < part->numIndices; i += 3)
            t3d_tri_draw(part->indices[i], part->indices[i+1], part->indices[i+2]);

        if (part->idxSeqCount)
            t3d_tri_draw_unindexed(part->idxSeqBase, part->idxSeqCount);

        for (int s = 0; s < 4; s++) {
            if (!part->numStripIndices[s]) break;
            t3d_tri_draw_strip(part->strips[s], part->numStripIndices[s]);
        }
        t3d_tri_sync();
    }
}

void t3d_model_draw_custom(const T3DModel *model, T3DModelDrawConf conf)
{
    assertf(model != NULL, "t3d_model_draw_custom: NULL model");
    T3DModelIter it = t3d_model_iter_create(model, T3D_CHUNK_TYPE_OBJECT);
    while (t3d_model_iter_next(&it)) {
            if (conf.filterCb && !conf.filterCb(conf.userData, it.object)) continue;
        if (it.object->material) t3d_model_draw_material(it.object->material, NULL);
        t3d_model_draw_object(it.object, conf.matrices);
    }
}

void t3d_model_draw(const T3DModel *model)
{
    T3DModelDrawConf conf;
    memset(&conf, 0, sizeof conf);
    t3d_model_draw_custom(model, conf);
}

void t3d_model_draw_skinned(const T3DModel *model, const T3DSkeleton *skeleton)
{
    (void)model; (void)skeleton;
    assertf(0, "t3d_model_draw_skinned is not implemented on the host: "
               "skinning needs t3dskeleton, which is types-only. A silent "
               "fallback to the base pose would look like the animation not "
               "playing.");
}

/* ── skeleton and animation: loud, on purpose ─────────────────────────── */

#define NOT_IMPL(what) \
    assertf(0, what " is not implemented in the host 3D pass. See " \
                    "plat/host/include/t3d/t3dskeleton.h for why this aborts " \
                    "rather than doing nothing.")

T3DSkeleton t3d_skeleton_create(const T3DModel *m) { (void)m; NOT_IMPL("t3d_skeleton_create"); return (T3DSkeleton){ 0 }; }
T3DSkeleton t3d_skeleton_create_buffered(const T3DModel *m, int n) { (void)m; (void)n; NOT_IMPL("t3d_skeleton_create_buffered"); return (T3DSkeleton){ 0 }; }
T3DSkeleton t3d_skeleton_clone(const T3DSkeleton *s, bool u) { (void)s; (void)u; NOT_IMPL("t3d_skeleton_clone"); return (T3DSkeleton){ 0 }; }
void t3d_skeleton_destroy(T3DSkeleton *s) { (void)s; }
void t3d_skeleton_reset(T3DSkeleton *s) { (void)s; NOT_IMPL("t3d_skeleton_reset"); }
void t3d_skeleton_update(T3DSkeleton *s) { (void)s; NOT_IMPL("t3d_skeleton_update"); }
void t3d_skeleton_use(const T3DSkeleton *s) { (void)s; NOT_IMPL("t3d_skeleton_use"); }
void t3d_skeleton_blend(const T3DSkeleton *o, const T3DSkeleton *a,
                        const T3DSkeleton *b, float f)
{ (void)o; (void)a; (void)b; (void)f; NOT_IMPL("t3d_skeleton_blend"); }

T3DAnim t3d_anim_create(const T3DModel *m, const char *n) { (void)m; (void)n; NOT_IMPL("t3d_anim_create"); return (T3DAnim){ 0 }; }
void t3d_anim_destroy(T3DAnim *a) { (void)a; }
void t3d_anim_attach(T3DAnim *a, const T3DSkeleton *s) { (void)a; (void)s; NOT_IMPL("t3d_anim_attach"); }
void t3d_anim_update(T3DAnim *a, float dt) { (void)a; (void)dt; NOT_IMPL("t3d_anim_update"); }
void t3d_anim_set_looping(T3DAnim *a, bool l) { if (a) a->isLooping = l; }
void t3d_anim_set_playing(T3DAnim *a, bool p) { if (a) a->isPlaying = p; }
void t3d_anim_set_time(T3DAnim *a, float t) { if (a) a->time = t; }
void t3d_anim_set_speed(T3DAnim *a, float s) { if (a) a->speed = s; }
bool t3d_anim_is_playing(const T3DAnim *a) { return a && a->isPlaying; }
