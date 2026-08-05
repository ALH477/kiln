/* SPDX-License-Identifier: MPL-2.0
 *
 * m64_cache.c — reference-counted resource cache implementation.
 */
#include "m64_cache.h"
#include <string.h>
#include <libdragon.h>

#define HANDLE_INDEX(h)   ((h) & 0xFFFFu)
#define HANDLE_GEN(h)     ((uint8_t)((h) >> 16))
#define MAKE_HANDLE(i, g) ((M64CacheHandle)((uint32_t)(i) | ((uint32_t)(g) << 16)))

void m64_cache_init(M64Cache *cache)
{
    memset(cache, 0, sizeof(*cache));
}

static int find_entry(const M64Cache *cache, const char *key)
{
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].refcount > 0 &&
            strcmp(cache->entries[i].key, key) == 0)
            return i;
    }
    return -1;
}

static int find_free(const M64Cache *cache)
{
    for (int i = 0; i < M64_CACHE_MAX_ENTRIES; i++) {
        if (cache->entries[i].refcount == 0)
            return i;
    }
    return -1;
}

M64CacheHandle m64_cache_acquire(M64Cache *cache,
                                 const char *key,
                                 M64CacheLoadFn load_fn,
                                 M64CacheReleaseFn release_fn,
                                 void *user_ctx)
{
    /* Already cached? Bump refcount. */
    int idx = find_entry(cache, key);
    if (idx >= 0) {
        cache->entries[idx].refcount++;
        return MAKE_HANDLE(idx, cache->entries[idx].generation);
    }

    /* Find a free slot. */
    idx = find_free(cache);
    if (idx < 0) {
        debugf("m64_cache: full (%d entries), cannot load '%s'\n",
               M64_CACHE_MAX_ENTRIES, key);
        return M64_CACHE_HANDLE_INVALID;
    }

    /* Load the resource. */
    void *resource = load_fn ? load_fn(key, user_ctx) : NULL;
    if (!resource) return M64_CACHE_HANDLE_INVALID;

    /* Populate the entry. */
    M64CacheEntry *e = &cache->entries[idx];
    size_t klen = strlen(key);
    if (klen >= M64_CACHE_MAX_KEY_LEN) klen = M64_CACHE_MAX_KEY_LEN - 1;
    memcpy(e->key, key, klen);
    e->key[klen] = '\0';
    e->resource = resource;
    e->refcount = 1;
    /* generation was incremented on the previous occupant's release (or 0) */

    if (idx >= cache->count) cache->count = idx + 1;

    return MAKE_HANDLE(idx, e->generation);
}

int m64_cache_release(M64Cache *cache,
                      M64CacheHandle handle,
                      M64CacheReleaseFn release_fn,
                      void *user_ctx)
{
    if (handle == M64_CACHE_HANDLE_INVALID) return -1;

    int idx = HANDLE_INDEX(handle);
    if (idx >= M64_CACHE_MAX_ENTRIES) return -1;

    M64CacheEntry *e = &cache->entries[idx];
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

void *m64_cache_resolve(const M64Cache *cache, M64CacheHandle handle)
{
    if (handle == M64_CACHE_HANDLE_INVALID) return NULL;
    int idx = HANDLE_INDEX(handle);
    if (idx >= M64_CACHE_MAX_ENTRIES) return NULL;
    const M64CacheEntry *e = &cache->entries[idx];
    if (e->refcount == 0) return NULL;
    if (e->generation != HANDLE_GEN(handle)) return NULL;
    return e->resource;
}

uint16_t m64_cache_refcount(const M64Cache *cache, M64CacheHandle handle)
{
    if (handle == M64_CACHE_HANDLE_INVALID) return 0;
    int idx = HANDLE_INDEX(handle);
    if (idx >= M64_CACHE_MAX_ENTRIES) return 0;
    const M64CacheEntry *e = &cache->entries[idx];
    if (e->generation != HANDLE_GEN(handle)) return 0;
    return e->refcount;
}