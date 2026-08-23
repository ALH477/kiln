/* SPDX-License-Identifier: MIT
 *
 * kiln_cache.h — reference-counted resource cache for shared assets.
 *
 * ── Why reference counting ─────────────────────────────────────────────
 * Multiple tiles may reference the same mesh, material, or texture. Without
 * a cache, each tile loads its own copy — wasting RDRAM on duplicates. Without
 * reference counting, the first tile to unload would free the resource while
 * others still reference it. A refcounted cache solves both: the first load
 * creates the resource, subsequent loads bump the refcount, and the resource
 * is freed only when the last reference is released.
 *
 * ── Why a flat array, not a hash table ────────────────────────────────
 * The upstream Junkrunner64 uses an open-addressing hash table with
 * linear probing and resizing. That is the right design for a host with
 * dynamic allocation and hundreds of assets. On the N64 with a bounded
 * asset set (typically 32–128 entries), a flat array with linear scan is
 * cheaper: no hash computation, no probing, no resize, and the entries
 * fit in a single cache line sweep. The O(n) scan is O(128) at worst,
 * which is 128 strcmp calls per cache lookup — under 1 ms on a VR4300.
 *
 * ── Generation counters ───────────────────────────────────────────────
 * A `KilnCacheHandle` packs the entry index and a generation counter into a
 * uint32_t. If an entry is freed and its slot is reused, the generation
 * increments, so a stale handle from a previous occupant resolves to NULL
 * instead of a dangling pointer. This is the same pattern as KilnActorHandle.
 *
 * ── What this is NOT ──────────────────────────────────────────────────
 * Not an LRU cache. Not a streaming cache. Not aware of StreamDB or DFS.
 * It is a pure refcounted lookup table: the caller provides the key (a
 * string), the cache returns a handle + resource pointer, and the resource
 * is freed when its refcount hits zero. The actual loading is done by a
 * caller-supplied callback so the cache is I/O-agnostic.
 *
 * Inspired by the resource_cache pattern in lambertjamesd/n64brew2025 but
 * designed from first principles: flat array instead of hash table,
 * generation-counted handles instead of raw pointers, and a load callback
 * instead of hardcoded asset_fopen.
 */
#ifndef KILN_CACHE_H
#define KILN_CACHE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KILN_CACHE_MAX_ENTRIES
/** Maximum simultaneously-cached resources. 128 covers a large overworld
 *  with shared meshes and materials. Override before including the header. */
#define KILN_CACHE_MAX_ENTRIES 128
#endif

/** Maximum key length. Keys are null-terminated strings (e.g. DFS paths
 *  or StreamDB keys). 63 chars covers "tiles/12_34/lod0.geom" comfortably. */
#define KILN_CACHE_MAX_KEY_LEN 64

/** A handle that survives entry reuse. Pack index + generation into a
 *  uint32_t: bits 0–15 = index, bits 16–23 = generation. */
typedef uint32_t KilnCacheHandle;

#define KILN_CACHE_HANDLE_INVALID 0u

/** One cache entry. */
typedef struct {
    char     key[KILN_CACHE_MAX_KEY_LEN];
    void    *resource;       /**< the loaded asset, or NULL if free       */
    uint16_t refcount;       /**< 0 means the slot is free                 */
    uint8_t  generation;    /**< incremented on each reuse               */
    uint8_t  _pad;
} KilnCacheEntry;

/** The cache. Embed by value. */
typedef struct {
    KilnCacheEntry entries[KILN_CACHE_MAX_ENTRIES];
    uint16_t count;          /**< high-water mark (entries ever used)      */
} KilnCache;

/** Load callback: the caller provides the actual loading. Receives the key
 *  and user_ctx; returns the loaded resource pointer, or NULL on failure.
 *  The cache takes ownership of the returned pointer — it will be freed via
 *  the release callback when refcount hits zero. */
typedef void *(*KilnCacheLoadFn)(const char *key, void *user_ctx);

/** Release callback: called when refcount reaches zero. The caller frees
 *  the resource (e.g. t3d_model_free, free, etc.). */
typedef void (*KilnCacheReleaseFn)(void *resource, void *user_ctx);

/** Initialise the cache (zero all entries). */
void kiln_cache_init(KilnCache *cache);

/** Look up or load a resource by key. If the key is already cached, bumps
 *  the refcount and returns the existing handle. If not, calls load_fn to
 *  create the resource, stores it, and returns a new handle. Returns
 *  KILN_CACHE_HANDLE_INVALID if the cache is full or load_fn returned NULL.
 *
 *  `user_ctx` is passed through to load_fn and release_fn unchanged. */
KilnCacheHandle kiln_cache_acquire(KilnCache *cache,
                                 const char *key,
                                 KilnCacheLoadFn load_fn,
                                 KilnCacheReleaseFn release_fn,
                                 void *user_ctx);

/** Release a reference. Decrements the refcount; if it reaches zero, calls
 *  release_fn on the resource and frees the slot. Returns 1 if the resource
 *  was freed, 0 if other references remain. Returns -1 if the handle is
 *  invalid (stale or never acquired). */
int kiln_cache_release(KilnCache *cache,
                      KilnCacheHandle handle,
                      KilnCacheReleaseFn release_fn,
                      void *user_ctx);

/** Resolve a handle to a resource pointer. Returns NULL if the handle is
 *  stale (generation mismatch) or the slot is free. */
void *kiln_cache_resolve(const KilnCache *cache, KilnCacheHandle handle);

/** Get the current refcount for a handle (0 if invalid). */
uint16_t kiln_cache_refcount(const KilnCache *cache, KilnCacheHandle handle);

/** Number of entries currently in use. */
static inline uint16_t kiln_cache_count(const KilnCache *cache) {
    uint16_t n = 0;
    for (int i = 0; i < KILN_CACHE_MAX_ENTRIES; i++)
        if (cache->entries[i].refcount > 0) n++;
    return n;
}

#ifdef __cplusplus
}
#endif

#endif /* KILN_CACHE_H */