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
static inline float rdf32(const uint8_t *b, size_t o)
{
    /* Through a union, not a pointer cast: the bytes arrive already assembled
     * host-endian by rd32, and the only thing left is to reinterpret them. A
     * cast would be a strict-aliasing violation the differential builds run
     * with -O2 are entitled to act on. */
    union { uint32_t u; float f; } c = { .u = rd32(b, o) };
    return c.f;
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
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    /* The file is big-endian because the console is. A big-endian host wants a
     * copy, not a swap — swapping here would corrupt exactly the targets that
     * agree with the console's byte order. Every other reader in this file
     * goes through rd16/rd32, which assemble byte-by-byte and are already
     * neutral; this is the one place that moves whole structs. */
    memcpy(d, s, 32);
#else
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
#endif
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
    T3DChunkSkeleton *skeleton;  /* parsed; NULL when the model has no 'S'  */
    T3DChunkAnim     *anims;     /* one per 'A' chunk, in file order        */
    uint32_t          animCount;
    T3DAnimChannelMapping *animMaps;  /* one flat pool for every clip       */
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

    /* ── the skeleton chunk ──
     * Layout confirmed against bytes, not just against Tiny3D's header: the
     * 23 bones of centaur.t3dm start at 0x10b20 and 4 + 23*48 lands on 0x10f74,
     * which is stringTablePtr exactly. So the header is four bytes and each
     * bone is 48 — a four-byte name offset, two uint16s, then three, four and
     * three floats. On the console this chunk is cast in place and only `name`
     * is patched; here every field is read out, because a host pointer is
     * eight bytes and the file reserves four. */
    for (uint32_t i = 0; i < M->chunkCount; i++) {
        if (M->chunks[i].type != T3D_CHUNK_TYPE_SKELETON) continue;
        assertf(M->skeleton == NULL,
                "t3dm: two skeleton chunks. Tiny3D's t3d_model_get_skeleton "
                "returns the first, so a second would be silently ignored.");
        const uint32_t o = M->chunks[i].off;
        assertf(o + 4u <= (uint32_t)size, "t3dm: skeleton chunk header runs past "
                "the %d-byte file", size);
        const uint32_t bc = rd16(b, o);
        assertf(o + 4u + bc * 48u <= (uint32_t)size,
                "t3dm: %u bones from %#x run past the %d-byte file", bc, o, size);
        M->skeleton = calloc(1, sizeof *M->skeleton
                                + (size_t)bc * sizeof(T3DChunkBone));
        assertf(M->skeleton != NULL, "t3dm: out of memory");
        M->skeleton->boneCount = (uint16_t)bc;
        for (uint32_t j = 0; j < bc; j++) {
            const uint32_t bo = o + 4u + j * 48u;
            T3DChunkBone *bone = &M->skeleton->bones[j];
            bone->name      = strtab_at(M, rd32(b, bo));
            bone->parentIdx = rd16(b, bo + 0x04);
            bone->depth     = rd16(b, bo + 0x06);
            for (int k = 0; k < 3; k++)
                bone->scale.v[k]    = rdf32(b, bo + 0x08 + 4u * (size_t)k);
            for (int k = 0; k < 4; k++)
                bone->rotation.v[k] = rdf32(b, bo + 0x14 + 4u * (size_t)k);
            for (int k = 0; k < 3; k++)
                bone->position.v[k] = rdf32(b, bo + 0x24 + 4u * (size_t)k);
            /* 0xFFFF is "no parent" and t3d_skeleton_update tests for exactly
             * that value; any other index must be a real bone, and must come
             * EARLIER, because the update walk composes parents in one forward
             * pass and would otherwise read an uninitialised matrix. */
            assertf(bone->parentIdx == 0xFFFF || bone->parentIdx < j,
                    "t3dm: bone %u ('%s') parent is %u — not 0xFFFF and not an "
                    "earlier bone, so a single forward pass cannot compose it",
                    j, bone->name ? bone->name : "?", bone->parentIdx);
        }
    }

    /* ── the animation chunks ──
     * 20 bytes then (channelsQuat + channelsScalar) x 12. Confirmed the same
     * way: centaur.t3dm's thirteen clips sit 0x428 apart and 20 + (23+64)*12
     * is 1064 = 0x428. The keyframes themselves are NOT here — `filePath`
     * names a sidecar ("rom:/models/centaur.0.sdata"), which is why animation
     * on the host is staged separately from the skeleton.
     */
    for (uint32_t i = 0; i < M->chunkCount; i++)
        if (M->chunks[i].type == T3D_CHUNK_TYPE_ANIM) M->animCount++;
    if (M->animCount) {
        M->anims = calloc(M->animCount, sizeof *M->anims);
        assertf(M->anims != NULL, "t3dm: out of memory");
        uint32_t totalMaps = 0, n = 0;
        for (uint32_t i = 0; i < M->chunkCount; i++) {
            if (M->chunks[i].type != T3D_CHUNK_TYPE_ANIM) continue;
            const uint32_t o = M->chunks[i].off;
            assertf(o + 20u <= (uint32_t)size,
                    "t3dm: anim chunk header at %#x runs past the %d-byte file",
                    o, size);
            T3DChunkAnim *a = &M->anims[n++];
            a->name           = strtab_at(M, rd32(b, o));
            a->duration       = rdf32(b, o + 0x04);
            a->keyframeCount  = rd32(b, o + 0x08);
            a->channelsQuat   = rd16(b, o + 0x0C);
            a->channelsScalar = rd16(b, o + 0x0E);
            a->filePath       = strtab_at(M, rd32(b, o + 0x10));
            /* fig_skel_set_phase divides by t3d_anim_get_length, which is
             * this field. A zero here is a divide by zero one frame later,
             * somewhere with no clue about where it came from. */
            assertf(a->duration > 0.0f,
                    "t3dm: animation '%s' has duration %f. fig_skel_set_phase "
                    "divides by it.", a->name ? a->name : "?", a->duration);
            totalMaps += (uint32_t)a->channelsQuat + a->channelsScalar;
        }
        if (totalMaps) {
            M->animMaps = calloc(totalMaps, sizeof *M->animMaps);
            assertf(M->animMaps != NULL, "t3dm: out of memory");
            uint32_t cur = 0; n = 0;
            for (uint32_t i = 0; i < M->chunkCount; i++) {
                if (M->chunks[i].type != T3D_CHUNK_TYPE_ANIM) continue;
                const uint32_t o = M->chunks[i].off;
                T3DChunkAnim *a = &M->anims[n++];
                const uint32_t cnt = (uint32_t)a->channelsQuat + a->channelsScalar;
                assertf(o + 20u + cnt * 12u <= (uint32_t)size,
                        "t3dm: animation '%s' channel mappings run past the "
                        "%d-byte file", a->name ? a->name : "?", size);
                a->channelMappings = &M->animMaps[cur];
                for (uint32_t k = 0; k < cnt; k++) {
                    const uint32_t mo = o + 20u + k * 12u;
                    T3DAnimChannelMapping *m = &a->channelMappings[k];
                    m->targetIdx    = rd16(b, mo);
                    m->targetType   = b[mo + 0x02];
                    m->attributeIdx = b[mo + 0x03];
                    m->quantScale   = rdf32(b, mo + 0x04);
                    m->quantOffset  = rdf32(b, mo + 0x08);
                }
                cur += cnt;
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
    /* A DFS path goes through the host VFS, like every other loader: it
     * resolves against the game's asset root, and it demands dfs_init, which
     * the console does too. A plain path is a host check reading a file it
     * just converted, and stays a plain fopen. */
    if (strncmp(path, "rom:", 4) == 0) {
        const int fd = dfs_open(path);
        assertf(fd > 0, "t3d_model_load: cannot open '%s'", path);
        const int sz = dfs_size(fd);
        assertf(sz > 0, "t3d_model_load: '%s' is empty", path);
        void *buf = malloc((size_t)sz);
        assertf(buf != NULL, "t3d_model_load: out of memory");
        assertf(dfs_read(buf, 1, sz, fd) == sz, "t3d_model_load: short read on '%s'", path);
        dfs_close(fd);
        return t3d_model_load_buf(buf, sz);
    }
    FILE *f = fopen(path, "rb");
    assertf(f != NULL, "t3d_model_load: cannot open '%s'", path);
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    assertf(sz > 0, "t3d_model_load: '%s' is empty", path);
    void *buf = malloc((size_t)sz);
    assertf(buf != NULL, "t3d_model_load: out of memory");
    assertf(fread(buf, 1, (size_t)sz, f) == (size_t)sz,
            "t3d_model_load: short read on '%s'", path);
    fclose(f);
    return t3d_model_load_buf(buf, (int)sz);
}

void t3d_model_free(T3DModel *model)
{
    if (!model) return;
    Model *M = as_model(model);
    free(M->stripPool); free(M->verts);
    free(M->parts); free(M->objects); free(M->materials); free(M->chunks);
    free(M->animMaps); free(M->anims); free(M->skeleton);
    free(M->raw);
    free(M);
}

/* ── accessors ────────────────────────────────────────────────────────── */

T3DVertPacked *t3d_model_get_vertices(const T3DModel *model)
{
    /* The host-endian copy, not the file bytes — a caller writing through this
     * (fig_vanim's morph path) must see the same representation the drawing
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
     * can stream vertices in (fig_vanim's morph path). Nothing in the host
     * drawing path acts on it yet, and quietly doing nothing would make a
     * morph render its base pose — which looks like the animation not
     * playing. Refuse instead. */
    (void)obj;
    assertf(0, "t3d_model_make_object_vert_placeholder is not implemented on "
               "the host: fig_vanim's morph path has no host equivalent yet.");
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
        } else if (iter->_chunkType == T3D_CHUNK_TYPE_SKELETON) {
            iter->skeleton = M->skeleton;
        } else if (iter->_chunkType == T3D_CHUNK_TYPE_ANIM) {
            uint32_t n = 0;
            for (uint32_t k = 0; k < i; k++)
                if (M->chunks[k].type == T3D_CHUNK_TYPE_ANIM) n++;
            iter->anim = &M->anims[n];
        } else {
            /* 'V', 'I' and 'B': handed out raw, as the file's bytes, because
             * nothing here parses them and the caller that asked for one by
             * type is reaching past this reader on purpose. The skeleton and
             * anim arms above exist because those two DO have parsed forms —
             * handing out the raw bytes cast to a struct whose first member is
             * an eight-byte pointer would have read every later field from the
             * wrong offset, and silently. */
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
     * what fig_scene_begin's RDPQ_COMBINER_SHADE means for an untextured
     * model, and is why nix/checks/kiln-voxmesh.nix exists. */
    (void)mat;
}

static bool handle_bone_matrix(const T3DObjectPart *part,
                               const T3DMat4FP *matStack, bool hadMatrixPush)
{
    /* Tiny3D's t3dmodel.c:164, line for line. The push/set distinction is not
     * cosmetic: push MULTIPLIES onto the current matrix (the model's world
     * transform), and set(.., true) replaces the top of the stack with that
     * same product, so consecutive bones each compose against the world
     * matrix rather than against each other. Pushing twice would accumulate. */
    if (matStack) {
        if (part->matrixIdx != 0xFFFF) {
            if (!hadMatrixPush) t3d_matrix_push((T3DMat4FP *)&matStack[part->matrixIdx]);
            else                t3d_matrix_set((T3DMat4FP *)&matStack[part->matrixIdx], true);
            hadMatrixPush = true;
        } else if (hadMatrixPush) {
            t3d_matrix_pop(1);
            hadMatrixPush = false;
        }
    }
    return hadMatrixPush;
}

void t3d_model_draw_object(const T3DObject *object, const T3DMat4FP *boneMatrices)
{
    assertf(object != NULL, "t3d_model_draw_object: NULL object");
    bool hadMatrixPush = false;
    for (uint32_t p = 0; p < object->numParts; p++) {
        const T3DObjectPart *part = &object->parts[p];
        hadMatrixPush = handle_bone_matrix(part, boneMatrices, hadMatrixPush);
        /* The load is what poses the vertices — see host_t3d.c's note on the
         * vertex cache holding TRANSFORMED vertices. So the bone matrix has to
         * be current BEFORE this line, and a part that loads nothing this
         * iteration still needs its matrix set for the part that draws its
         * slice later. */
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
    if (hadMatrixPush) t3d_matrix_pop(1);
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

/* ── skinning ─────────────────────────────────────────────────────────────
 * This used to abort, and the abort's reasoning was sound while it stood:
 * "a silent fallback to the base pose would look like the animation not
 * playing". What changed is that the skeleton is now real, so there is no
 * fallback — the bone matrices handed to the draw are the ones the console
 * would compute, from the same chunk, through the same t3d_mat4_from_srt.
 *
 * Rigid skinning needs nothing from the rasteriser, only from the ORDER of
 * operations, and host_t3d.c's vertex cache already has it: t3d_vert_load
 * transforms at LOAD time against the current matrix, so pushing a bone's
 * matrix before loading that bone's part puts its vertices into the cache
 * already posed. That comment ("the semantic that rigid skinning rests on")
 * was written before anything exercised it. Now something does.
 *
 * What is still staged is the ANIMATION: the keyframe stream lives outside
 * the .t3dm in a .sdata sidecar, so t3d_anim_update keeps the clock and
 * leaves the bones alone. A skinned character therefore stands in its bind
 * pose while every clip's timing — phase, length, loop wrap, "is it done" —
 * is exactly the console's. That is the half this native loop exists to test,
 * and the other half says so once per clip rather than silently.
 */

void t3d_model_draw_skinned(const T3DModel *model, const T3DSkeleton *skeleton)
{
    assertf(model != NULL && skeleton != NULL, "t3d_model_draw_skinned: NULL");
    /* Tiny3D picks t3d_segment_placeholder for a multi-buffered skeleton,
     * because the RSP resolves the segment at DMA time. There is no RSP here
     * and the placeholder is not a dereferenceable address, so refuse rather
     * than follow it. Nothing reaches this today: fig_skel_create calls
     * t3d_skeleton_create, which is bufferCount 1. */
    assertf(skeleton->bufferCount == 1,
            "t3d_model_draw_skinned: bufferCount %d. The console indirects "
            "through T3D_SEGMENT_SKELETON here, which the host cannot follow. "
            "Use t3d_skeleton_create (one buffer) or teach host_t3d.c real "
            "segments.", skeleton->bufferCount);
    T3DModelDrawConf conf;
    memset(&conf, 0, sizeof conf);
    conf.matrices = skeleton->boneMatricesFP;
    t3d_model_draw_custom(model, conf);
}

/* ── skeleton ─────────────────────────────────────────────────────────── */

static const T3DChunkSkeleton *skeleton_of(const T3DModel *model)
{
    assertf(model != NULL, "t3d_skeleton_create: NULL model");
    const T3DChunkSkeleton *ref = as_model(model)->skeleton;
    assertf(ref != NULL,
            "t3d_skeleton_create: this model has no 'S' chunk. gltf_to_t3d "
            "only writes one for a glTF with an armature — an unskinned model "
            "has no skeleton to create.");
    return ref;
}

T3DSkeleton t3d_skeleton_create_buffered(const T3DModel *model, int bufferCount)
{
    const T3DChunkSkeleton *ref = skeleton_of(model);
    assertf(bufferCount >= 1, "t3d_skeleton_create_buffered: bufferCount %d",
            bufferCount);
    T3DSkeleton skel = (T3DSkeleton){
        .bones          = calloc(ref->boneCount, sizeof(T3DBone)),
        .boneMatricesFP = calloc((size_t)ref->boneCount * (size_t)bufferCount,
                                 sizeof(T3DMat4FP)),
        .bufferCount    = (uint8_t)bufferCount,
        .currentBufferIdx = 0,
        .skeletonRef    = ref,
    };
    assertf(skel.bones && skel.boneMatricesFP,
            "t3d_skeleton_create_buffered: out of memory");
    t3d_skeleton_reset(&skel);
    return skel;
}

T3DSkeleton t3d_skeleton_create(const T3DModel *model)
{
    return t3d_skeleton_create_buffered(model, 1);
}

T3DSkeleton t3d_skeleton_clone(const T3DSkeleton *skel, bool useMatrices)
{
    assertf(skel != NULL && skel->skeletonRef != NULL,
            "t3d_skeleton_clone: NULL or uninitialised skeleton");
    const uint16_t n = skel->skeletonRef->boneCount;
    T3DSkeleton out = (T3DSkeleton){
        .bones          = calloc(n, sizeof(T3DBone)),
        .boneMatricesFP = NULL,
        .skeletonRef    = skel->skeletonRef,
    };
    assertf(out.bones != NULL, "t3d_skeleton_clone: out of memory");
    memcpy(out.bones, skel->bones, (size_t)n * sizeof(T3DBone));
    if (useMatrices) {
        /* Upstream copies bufferCount matrices but leaves out.bufferCount at
         * zero, which is its bug and not one to reproduce blindly — a clone
         * with matrices and bufferCount 0 makes t3d_skeleton_update's modulo a
         * divide by zero. fig_skel only ever clones with `false`
         * (kiln_skel.c:21,82), so carry the count and say so here. */
        const size_t bytes = (size_t)n * skel->bufferCount * sizeof(T3DMat4FP);
        out.boneMatricesFP = calloc(1, bytes ? bytes : 1);
        assertf(out.boneMatricesFP != NULL, "t3d_skeleton_clone: out of memory");
        memcpy(out.boneMatricesFP, skel->boneMatricesFP, bytes);
        out.bufferCount = skel->bufferCount;
        out.currentBufferIdx = skel->currentBufferIdx;
    }
    return out;
}

void t3d_skeleton_destroy(T3DSkeleton *skel)
{
    if (!skel) return;
    free(skel->bones);          skel->bones = NULL;
    free(skel->boneMatricesFP); skel->boneMatricesFP = NULL;
    skel->skeletonRef = NULL;
}

void t3d_skeleton_reset(T3DSkeleton *skel)
{
    assertf(skel && skel->skeletonRef, "t3d_skeleton_reset: NULL");
    for (uint16_t i = 0; i < skel->skeletonRef->boneCount; i++) {
        const T3DChunkBone *def = &skel->skeletonRef->bones[i];
        /* Upstream is one memcpy over the whole SRT triple, relying on scale,
         * rotation and position being adjacent in BOTH structs. They are, but
         * the field order differs between T3DBone (matrix first) and
         * T3DChunkBone (name, parentIdx, depth first), so the copy is written
         * out per member here. Same values, no layout assumption. */
        skel->bones[i].scale      = def->scale;
        skel->bones[i].rotation   = def->rotation;
        skel->bones[i].position   = def->position;
        skel->bones[i].hasChanged = true;
    }
}

void t3d_skeleton_blend(const T3DSkeleton *out, const T3DSkeleton *a,
                        const T3DSkeleton *b, float factor)
{
    assertf(out && a && b && out->skeletonRef, "t3d_skeleton_blend: NULL");
    assertf(a->skeletonRef == out->skeletonRef && b->skeletonRef == out->skeletonRef,
            "t3d_skeleton_blend: blending skeletons from different models");
    for (uint16_t i = 0; i < out->skeletonRef->boneCount; i++) {
        T3DBone *r = &out->bones[i];
        r->hasChanged = true;
        t3d_quat_nlerp(&r->rotation, &a->bones[i].rotation, &b->bones[i].rotation, factor);
        t3d_vec3_lerp(&r->position, &a->bones[i].position, &b->bones[i].position, factor);
        t3d_vec3_lerp(&r->scale,    &a->bones[i].scale,    &b->bones[i].scale,    factor);
    }
}

void t3d_skeleton_update(T3DSkeleton *skel)
{
    assertf(skel && skel->skeletonRef, "t3d_skeleton_update: NULL");
    assertf(skel->bufferCount >= 1,
            "t3d_skeleton_update: bufferCount 0 — the buffer cycle is a modulo "
            "by it. A clone made with useMatrices is the only way to get here.");
    /* Tiny3D's t3dskeleton.c, including the two things that look like they
     * could be dropped and cannot:
     *   - the forced walk of children after a changed bone, bounded by depth,
     *     which is why the parser insists a parent comes earlier;
     *   - hasChanged counting UP to bufferCount rather than clearing at once,
     *     so a settled pose still reaches every buffer before it stops being
     *     rewritten. With bufferCount 1 that is invisible; with 2 or 3 it is
     *     the difference between a still pose and a flickering one. */
    int updateLevel = -1;
    bool forceUpdate = false;
    T3DMat4FP *matStackFP = NULL;

    for (uint16_t i = 0; i < skel->skeletonRef->boneCount; i++) {
        T3DBone *bone = &skel->bones[i];
        const T3DChunkBone *def = &skel->skeletonRef->bones[i];

        if (forceUpdate && def->depth <= updateLevel) {
            forceUpdate = false;
            updateLevel = -1;
        }
        if (!bone->hasChanged && !forceUpdate) continue;

        if (matStackFP == NULL) {
            skel->currentBufferIdx = (uint8_t)((skel->currentBufferIdx + 1)
                                               % skel->bufferCount);
            matStackFP = &skel->boneMatricesFP[(size_t)skel->skeletonRef->boneCount
                                               * skel->currentBufferIdx];
        }
        if (!forceUpdate) updateLevel = def->depth;
        forceUpdate = true;

        if (def->parentIdx != 0xFFFF) {
            T3DMat4 tmp;
            t3d_mat4_from_srt(&tmp, bone->scale.v, bone->rotation.v, bone->position.v);
            t3d_mat4_mul(&bone->matrix, &skel->bones[def->parentIdx].matrix, &tmp);
        } else {
            t3d_mat4_from_srt(&bone->matrix, bone->scale.v, bone->rotation.v,
                              bone->position.v);
        }
        t3d_mat4_to_fixed(&matStackFP[i], &bone->matrix);

        if (bone->hasChanged++ == skel->bufferCount) bone->hasChanged = 0;
    }
}

void t3d_skeleton_use(const T3DSkeleton *skel)
{
    /* Upstream only touches a segment when bufferCount > 1, and the host's
     * t3d_segment_set is a no-op, so this is a no-op either way — but it is
     * the no-op the console also performs for bufferCount 1, not a stub. */
    assertf(skel != NULL, "t3d_skeleton_use: NULL");
    if (skel->bufferCount > 1)
        t3d_segment_set(T3D_SEGMENT_SKELETON,
                        &skel->boneMatricesFP[(size_t)skel->currentBufferIdx
                                              * skel->skeletonRef->boneCount]);
}

/* ── animation: the clock is real, the keyframes are not ───────────────── */

T3DAnim t3d_anim_create(const T3DModel *model, const char *name)
{
    assertf(model != NULL && name != NULL, "t3d_anim_create: NULL");
    Model *M = as_model(model);
    for (uint32_t i = 0; i < M->animCount; i++) {
        if (M->anims[i].name && strcmp(M->anims[i].name, name) == 0) {
            return (T3DAnim){
                .animRef   = &M->anims[i],
                .time      = 0.0f,
                .speed     = 1.0f,
                .isPlaying = 1,
                .isLooping = 1,
            };
        }
    }
    /* The console asserts here too ("Animation '%s' not found in model"), but
     * the host can afford to say what IS there, which turns a wrong clip name
     * from a guess into a reading. */
    char have[512] = "";
    for (uint32_t i = 0; i < M->animCount; i++) {
        if (strlen(have) + strlen(M->anims[i].name ? M->anims[i].name : "?") + 3
            >= sizeof have) { strcat(have, "..."); break; }
        if (i) strcat(have, ", ");
        strcat(have, M->anims[i].name ? M->anims[i].name : "?");
    }
    assertf(0, "t3d_anim_create: no animation '%s' in this model. It has %u: %s",
            name, M->animCount, M->animCount ? have : "(none)");
    return (T3DAnim){ 0 };
}

void t3d_anim_destroy(T3DAnim *a) { if (a) a->animRef = NULL; }

void t3d_anim_attach(T3DAnim *a, const T3DSkeleton *skeleton)
{
    /* On the console this allocates one target per channel and points each at
     * a bone's rotation/position/scale field, then rewinds the .sdata stream.
     * With no stream to read there is nothing to bind, and rewinding is what
     * t3d_anim_set_time already does. Attaching twice is legal upstream
     * (fig_skel re-attaches on every clip change), so this must not be an
     * abort. */
    assertf(a != NULL && skeleton != NULL, "t3d_anim_attach: NULL");
    a->time = 0.0f;
}

void t3d_anim_update(T3DAnim *a, float deltaTime)
{
    assertf(a != NULL, "t3d_anim_update: NULL");
    assertf(a->animRef != NULL, "t3d_anim_update: animation was never created");
    if (!a->isPlaying) return;

    /* Tiny3D's t3danim.c:170-184 exactly — everything upstream does BEFORE it
     * reaches the keyframe channels. So loop wrap, the non-looping stop, and
     * the time a clip reports are the console's, which is what fig_skel's
     * overlay fade (kiln_skel.c:263-267) and fig_skel_done read. */
    a->time += deltaTime * a->speed;
    if (a->time >= a->animRef->duration) {
        a->time -= a->animRef->duration;
        if (!a->isLooping) {
            a->isPlaying = 0;
            return;
        }
    }

    /* The channels are where the console poses bones from the .sdata stream.
     * Said once per clip, not per frame: 60 of these a second would bury the
     * text manifest this native loop exists to read. */
    static const T3DChunkAnim *told[64];
    static int toldCount;
    for (int i = 0; i < toldCount; i++) if (told[i] == a->animRef) return;
    if (toldCount < (int)(sizeof told / sizeof *told)) told[toldCount++] = a->animRef;
    debugf("host: animation '%s' keeps its clock but does not pose bones — "
           "%u keyframes live in %s, which the host does not stream yet. The "
           "model stands in its bind pose.\n",
           a->animRef->name ? a->animRef->name : "?", a->animRef->keyframeCount,
           a->animRef->filePath ? a->animRef->filePath : "(no sidecar)");
}

void t3d_anim_set_looping(T3DAnim *a, bool l) { if (a) a->isLooping = l; }
void t3d_anim_set_playing(T3DAnim *a, bool p) { if (a) a->isPlaying = p; }
void t3d_anim_set_time(T3DAnim *a, float t) { if (a) a->time = t; }
void t3d_anim_set_speed(T3DAnim *a, float s) { if (a) a->speed = s; }
bool t3d_anim_is_playing(const T3DAnim *a) { return a && a->isPlaying; }
