/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/t3d/t3dmodel.h — the host's <t3d/t3dmodel.h>.
 *
 * ── Why this cannot be the real header ────────────────────────────────
 * Tiny3D's model loader is an IN-PLACE RELOCATION: the .t3dm file is read into
 * memory and its structs ARE the file layout, with "pointers" stored as
 * offsets that get a base added to them. That works because the target is
 * 32-bit — a `char *name` field is four bytes in the file and four bytes in
 * the struct.
 *
 * On x86-64 a pointer is eight. So the file layout and the struct layout
 * cannot be the same thing, and reinterpreting the buffer in place would read
 * every field after the first pointer from the wrong offset. The host
 * therefore PARSES the file at its documented offsets and builds native
 * structs, rather than casting.
 *
 * That is a real divergence and worth naming: this is the first piece of the
 * host backend that is a reimplementation of a FORMAT rather than of an API.
 * nix/checks/kiln-model.nix is what keeps it honest — it parses the same
 * .t3dm the ROM links and asserts the counts against what gltf_to_t3d
 * reported.
 *
 * ── What is exposed ──────────────────────────────────────────────────
 * Only the fields the engine and PetaByte Madness actually read, which is a
 * short list: model->totalVertCount, obj->numParts, obj->parts,
 * obj->material, part->vert and part->vertLoadCount. Everything else the
 * drawing path needs is private to host_t3dmodel.c.
 */
#ifndef KILN_HOST_T3DMODEL_H
#define KILN_HOST_T3DMODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <t3d/t3d.h>

/* Chunk type tags, verbatim from Tiny3D — they are bytes in the file. */
enum T3DChunkType {
    T3D_CHUNK_TYPE_VERTICES = 'V',
    T3D_CHUNK_TYPE_INDICES  = 'I',
    T3D_CHUNK_TYPE_MATERIAL = 'M',
    T3D_CHUNK_TYPE_OBJECT   = 'O',
    T3D_CHUNK_TYPE_SKELETON = 'S',
    T3D_CHUNK_TYPE_ANIM     = 'A',
    T3D_CHUNK_TYPE_BVH      = 'B',
};

typedef struct {
    char    *name;
    uint32_t renderFlags;
    uint8_t  fogMode;
    char    *texPathA;
    char    *texPathB;
    /* The one field of Tiny3D's T3DMaterialTexture the engine reads:
     * kiln_texanim matches a texture-reference material by it. The host reader
     * leaves it 0, which Tiny3D also reads as "not a reference", and the host
     * draw never calls a dynTextureCb anyway. */
    struct { uint32_t texReference; } textureA;
} T3DMaterial;

typedef struct {
    T3DVertPacked *vert;          /* into the model's vertex chunk           */
    uint16_t vertLoadCount;
    uint16_t vertDestOffset;
    uint8_t *indices;             /* indexed triangles, 3 per triangle       */
    uint16_t numIndices;
    uint16_t matrixIdx;
    uint8_t  numStripIndices[4];
    uint8_t  idxSeqBase;
    uint8_t  idxSeqCount;
    /* Host-side: the raw strip index buffers, one per entry above. Kept
     * separately because the console reaches them by pointer arithmetic off
     * `indices` with 8-byte alignment, which is a file-layout detail rather
     * than something a caller should reproduce. */
    int16_t *strips[4];
} T3DObjectPart;

typedef struct {
    char        *name;
    uint16_t     numParts;
    uint16_t     triCount;
    T3DMaterial *material;
    uint8_t      isVisible;
    uint8_t      userValue0, userValue1;
    int16_t      aabbMin[3], aabbMax[3];
    T3DObjectPart *parts;         /* numParts entries                        */
} T3DObject;

/* The bone and skeleton chunks are the console's layouts, copied from
 * Tiny3D's t3dmodel.h: kiln_skel reads `name` and `depth` to build bone
 * masks, and that code has to compile here to be in HOST_MODULES. Nothing on
 * the host parses them — no .t3dm skeleton is ever loaded (see
 * t3dskeleton.h). */
typedef struct {
    char    *name;
    uint16_t parentIdx;
    uint16_t depth;
    T3DVec3  scale;
    T3DQuat  rotation;
    T3DVec3  position;
} T3DChunkBone;

typedef struct {
    uint16_t     boneCount;
    uint16_t     _reserved;
    T3DChunkBone bones[];
} T3DChunkSkeleton;

typedef struct {
    char    *name;
    float    duration;
    uint32_t keyframeCount;
} T3DChunkAnim;

typedef struct T3DModel T3DModel;
/* Forward-declared rather than included: t3dskeleton.h needs T3DModel, so
 * including it here would be circular. */
typedef struct T3DSkeleton_s T3DSkeleton;

typedef struct {
    union {
        void             *chunk;
        T3DObject        *object;
        T3DMaterial      *material;
        T3DChunkSkeleton *skeleton;
        T3DChunkAnim     *anim;
    };
    const T3DModel *_model;
    uint16_t        _idx;
    char            _chunkType;
} T3DModelIter;

/* Verbatim from Tiny3D, names included: kiln_texanim builds a
 * T3DModelDrawConf with designated initialisers, so the FIELD names are part
 * of the contract, and filterCb returns bool rather than void. */
typedef void (*T3DModelTileCb)(void *userData, rdpq_texparms_t *tileParams,
                               rdpq_tile_t tile);
typedef bool (*T3DModelFilterCb)(void *userData, const T3DObject *obj);
typedef void (*T3DModelDynTextureCb)(void *userData, const T3DMaterial *material,
                                     rdpq_texparms_t *tileParams,
                                     rdpq_tile_t tile);

typedef struct {
    void                *userData;
    T3DModelTileCb       tileCb;
    T3DModelFilterCb     filterCb;
    T3DModelDynTextureCb dynTextureCb;
    T3DMat4FP           *matrices;
} T3DModelDrawConf;

/* The parts of T3DModel the engine reads. The rest is opaque. */
struct T3DModel {
    char     magic[4];
    uint16_t totalVertCount;
    uint16_t totalIndexCount;
    int16_t  aabbMin[3];
    int16_t  aabbMax[3];
    /* host-private below */
    void      *_raw;
    uint32_t   _chunkCount;
    void      *_chunks;
    T3DObject *_objects;
    uint32_t   _objectCount;
};

T3DModel *t3d_model_load(const char *path);
T3DModel *t3d_model_load_buf(void *buf, int size);
void      t3d_model_free(T3DModel *model);

void t3d_model_draw(const T3DModel *model);
void t3d_model_draw_custom(const T3DModel *model, T3DModelDrawConf conf);
void t3d_model_draw_object(const T3DObject *object, const T3DMat4FP *boneMatrices);
void t3d_model_draw_material(T3DMaterial *mat, void *state);
void t3d_model_draw_skinned(const T3DModel *model, const T3DSkeleton *skeleton);

T3DObject *t3d_model_get_object(const T3DModel *model, const char *name);
T3DObject *t3d_model_get_object_by_index(const T3DModel *model, uint32_t index);
T3DVertPacked *t3d_model_get_vertices(const T3DModel *model);
void t3d_model_make_object_vert_placeholder(const T3DModel *model,
                                            T3DObject *object,
                                            uint8_t segmentId);

T3DModelIter t3d_model_iter_create(const T3DModel *model, enum T3DChunkType type);
bool         t3d_model_iter_next(T3DModelIter *iter);

/* Strip and sequence submission, defined in host_t3d.c beside t3d_tri_draw. */
void t3d_tri_draw_strip(int16_t *indexBuff, int count);
void t3d_tri_draw_strip_and_sync(int16_t *indexBuff, int count);
void t3d_tri_draw_unindexed(int base, int count);
void t3d_indexbuffer_convert(int16_t indices[], int count);

#endif /* KILN_HOST_T3DMODEL_H */
