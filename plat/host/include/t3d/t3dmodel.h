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
#ifndef FIG_HOST_T3DMODEL_H
#define FIG_HOST_T3DMODEL_H

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
     * fig_texanim matches a texture-reference material by it. The host reader
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

/* ── The bone, skeleton and animation chunks ──────────────────────────────
 * Copied from Tiny3D's t3dmodel.h, field for field. These are now PARSED by
 * host_t3dmodel.c and handed to a real t3d_skeleton_create, so every member
 * is load-bearing rather than decoration: fig_skel reads `name` and `depth`
 * to build bone masks, t3d_skeleton_reset copies the SRT triple as one
 * memcpy (so their ORDER and ADJACENCY are part of the contract), and
 * t3d_skeleton_update walks `parentIdx`, treating 0xFFFF as "no parent".
 *
 * These are host-native structs filled by the parser, NOT the file's bytes
 * reinterpreted. The file stores each pointer as a four-byte string-table
 * offset, so casting the chunk would read every field after `name` from the
 * wrong place — the same reason this file parses objects and materials
 * instead of casting them (host_t3dmodel.c's preamble has the long version).
 *
 * T3DChunkAnim carried only its first three members until the parser landed,
 * which was wrong in the way this file's own rule names: `channelsQuat`,
 * `channelsScalar` and `filePath` exist on the console, and the chunk's size
 * is 20 bytes plus 12 per (channelsQuat + channelsScalar) mapping — measured
 * against centaur.t3dm, whose thirteen clips sit exactly 0x428 = 20 + 87*12
 * bytes apart. A truncated copy made the stride unknowable.
 */
typedef struct {
    uint16_t targetIdx;
    uint8_t  targetType;
    uint8_t  attributeIdx;
    float    quantScale;
    float    quantOffset;
} T3DAnimChannelMapping;

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
    uint16_t channelsQuat;
    uint16_t channelsScalar;
    char    *filePath;
    /* Upstream's flexible `channelMappings[]` is a pointer here: the parser
     * allocates the mappings separately because the host struct is wider than
     * the file's, so they cannot follow it in place. Nothing in the engine
     * reads them yet; they are parsed so the keyframe stream has somewhere to
     * land when host animation stops being bind-pose-only. */
    T3DAnimChannelMapping *channelMappings;
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

/* Verbatim from Tiny3D, names included: fig_texanim builds a
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
    /* const, as Tiny3D has it (t3dmodel.h:227). It was not, and a caller
     * passing a `const T3DMat4FP *` — pm_veil.c:457 does — got a
     * -Wdiscarded-qualifiers error under this tree's -Werror on the host and
     * built fine for the console. Another copied definition that was not. */
    const T3DMat4FP     *matrices;
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
/* ── Draw state ───────────────────────────────────────────────────────────
 * Tiny3D threads this through a manual draw loop so consecutive objects do
 * not re-send a material the RDP already has. The host's draw_material
 * ignores it (its combiner is a set of named sentinels, not an RDP register
 * encoding — see host_t3dmodel.c), so here it is only ever read back.
 *
 * It is still the real struct rather than an opaque blob, because callers
 * read `lastVertFXFunc` to decide whether to clear the vertex-FX function on
 * the way out. On the host nothing sets it, so that epilogue correctly never
 * fires — which is the answer, not an omission. */
typedef struct {
    uint32_t lastTextureHashA;
    uint32_t lastTextureHashB;
    uint8_t  lastFogMode;
    uint32_t lastRenderFlags;
    uint64_t lastCC;
    color_t  lastPrimColor;
    color_t  lastEnvColor;
    color_t  lastBlendColor;
    uint8_t  lastVertFXFunc;
    uint16_t lastUvGenParams[2];
    uint64_t lastOtherMode;
    uint32_t lastBlendMode;
    void    *drawConf;
} T3DModelState;

static inline T3DModelState t3d_model_state_create(void) {
    return (T3DModelState){
        .lastFogMode = 0xFF,
        .lastVertFXFunc = T3D_VERTEX_FX_NONE,
        .lastOtherMode = 0xFF,
        .lastBlendMode = 0xFFFFFFFF,
    };
}

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

#endif /* FIG_HOST_T3DMODEL_H */
