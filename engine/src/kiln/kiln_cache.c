/* SPDX-License-Identifier: MIT
 *
 * kiln_cache.c — reference-counted resource cache implementation.
 */
#include "kiln_cache.h"
#include <string.h>
#include <libdragon.h>

/* ── The index is stored BIASED BY ONE, and that is load-bearing ────────
 * KILN_CACHE_HANDLE_INVALID is 0, and the packing used to be a plain
 * `index | generation << 16`. So slot 0 at generation 0 — which is exactly
 * what a FRESH cache hands out for the FIRST resource anyone acquires —
 * packed to 0 and was therefore indistinguishable from failure:
 *
 *   kiln_cache_acquire  returned 0, which every caller is told means "full or
 *                      the load failed", for a load that in fact succeeded.
 *   kiln_cache_resolve  early-returns NULL on 0, so the resource could never
 *                      be reached.
 *   kiln_cache_release  early-returns -1 on 0, so it could never be freed —
 *                      the slot stayed occupied for the rest of the session.
 *
 * The whole first slot was dead, and it failed in the shape of a missing
 * asset, which is the most misleading shape available: openworld-demo's first
 * tile would look like a file that was not in the ROM. Found by
 * nix/checks/kiln-logic.nix on its first run; the module had no test before
 * that, and no ROM screenshot would have distinguished it.
 *
 * Biasing by one costs nothing (the index field is 16 bits and
 * KILN_CACHE_MAX_ENTRIES is 128) and makes 0 unrepresentable as a live handle
 * rather than merely unlikely. */
#define HANDLE_INDEX(h)   ((int)((h) & 0xFFFFu) - 1)
#define HANDLE_GEN(h)     ((uint8_t)((h) >> 16))
#define MAKE_HANDLE(i, g) ((KilnCacheHandle)(((uint32_t)(i) + 1u) \
                                            | ((uint32_t)(g) << 16)))

void kiln_cache_init(KilnCache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

static int find_entry(const KilnCache *cache, const char *key)
{
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].refcount > 0 &&
            strcmp(cache->entries[i].key, key) == 0)
            return i;
    }
    return -1;
}

static int find_free(const KilnCache *cache)
{
    for (int i = 0; i < KILN_CACHE_MAX_ENTRIES; i++) {
        if (cache->entries[i].refcount == 0)
            return i;
    }
    return -1;
}

KilnCacheHandle kiln_cache_acquire(KilnCache *cache,
                                 const char *key,
                                 KilnCacheLoadFn load_fn,
                                 KilnCacheReleaseFn release_fn,
                                 void *user_ctx)
{
    /* Unused here, and deliberately still in the signature: acquire and
     * release are always called as a pair with the SAME pair of callbacks, and
     * taking both at the acquire site is what makes a mismatched release
     * visible when reading the call site rather than only at the release.
     * Marked rather than removed — and marked rather than left to warn,
     * because the engine builds with -Wno-error and an unused-parameter
     * warning nobody sees is how a genuinely unused argument hides. */
    (void)release_fn;

    /* Already cached? Bump refcount. */
    int idx = find_entry(cache, key);
    if (idx >= 0) {
        cache->entries[idx].refcount++;
        return MAKE_HANDLE(idx, cache->entries[idx].generation);
    }

    /* Find a free slot. */
    idx = find_free(cache);
    if (idx < 0) {
        debugf("kiln_cache: full (%d entries), cannot load '%s'\n",
               KILN_CACHE_MAX_ENTRIES, key);
        return KILN_CACHE_HANDLE_INVALID;
    }

    /* Load the resource. */
    void *resource = load_fn ? load_fn(key, user_ctx) : NULL;
    if (!resource) return KILN_CACHE_HANDLE_INVALID;

    /* Populate the entry. */
    KilnCacheEntry *e = &cache->entries[idx];
    size_t klen = strlen(key);
    if (klen >= KILN_CACHE_MAX_KEY_LEN) klen = KILN_CACHE_MAX_KEY_LEN - 1;
    memcpy(e->key, key, klen);
    e->key[klen] = '\0';
    e->resource = resource;
    e->refcount = 1;
    /* generation was incremented on the previous occupant's release (or 0) */

    if (idx >= cache->count) cache->count = idx + 1;

    return MAKE_HANDLE(idx, e->generation);
}

int kiln_cache_release(KilnCache *cache,
                      KilnCacheHandle handle,
                      KilnCacheReleaseFn release_fn,
                      void *user_ctx)
{
    if (handle == KILN_CACHE_HANDLE_INVALID) return -1;

    int idx = HANDLE_INDEX(handle);
    /* Both ends: the biased index can be -1 for a handle whose low 16
     * bits are zero but whose generation is not, which the
     * INVALID comparison above does not catch. */
    if (idx < 0 || idx >= KILN_CACHE_MAX_ENTRIES) return -1;

    KilnCacheEntry *e = &cache->entries[idx];
    if (e->refcount == 0) return -1;
    if (e->generation != HANDLE_GEN(handle)) return -1;

    e->refcount--;
    if (e->refcount > 0) return 0;

    /* Refcount hit zero: free the resource and mark the slot. */
    if (release_fn && e->resource)
        release_fn(e->resource, user_ctx);
    e->resource = NULL;
    e->generation++;  /* invalidate stale handles */
    return 1;
}

void *kiln_cache_resolve(const KilnCache *cache, KilnCacheHandle handle)
{
    if (handle == KILN_CACHE_HANDLE_INVALID) return NULL;
    int idx = HANDLE_INDEX(handle);
    if (idx < 0 || idx >= KILN_CACHE_MAX_ENTRIES) return NULL;
    const KilnCacheEntry *e = &cache->entries[idx];
    if (e->refcount == 0) return NULL;
    if (e->generation != HANDLE_GEN(handle)) return NULL;
    return e->resource;
}

uint16_t kiln_cache_refcount(const KilnCache *cache, KilnCacheHandle handle)
{
    if (handle == KILN_CACHE_HANDLE_INVALID) return 0;
    int idx = HANDLE_INDEX(handle);
    if (idx < 0 || idx >= KILN_CACHE_MAX_ENTRIES) return 0;
    const KilnCacheEntry *e = &cache->entries[idx];
    if (e->generation != HANDLE_GEN(handle)) return 0;
    return e->refcount;
}