/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * streamdb_embedded.h — StreamDB v3 reader for bare-metal / constrained targets.
 *
 * A read-only implementation of the DeMoD StreamDB v3 on-disk format
 * (github.com/ALH477/DeMoD-StreamDB) for machines with no operating system:
 * Nintendo 64 and similar. Files produced by the Rust or C editions are read
 * unmodified; this produces none.
 *
 * ── Why a separate implementation rather than a port ───────────────────
 * The upstream C edition cannot run here, for reasons that are structural
 * rather than incidental:
 *
 *   * It needs pthreads (a recursive mutex plus a background auto-flush
 *     thread), flock(), fsync()/fdatasync(), and stdio FILE*. On libdragon
 *     none of those exist.
 *   * Its in-memory trie node is `TrieNode *children[256]` — 1 KB per node on
 *     a 32-bit target, 2 KB on 64-bit. A few thousand assets would be several
 *     megabytes of index on a machine with 4 MB of RAM total. That single
 *     structural choice is fine on a desktop and fatal here.
 *   * Writing at all presumes a mutable, seekable, fsync-able file. A ROM is
 *     none of those things.
 *
 * So this is a reader with the same format and a different memory strategy:
 * the trie is parsed once into a **flat array of nodes with contiguous child
 * ranges**, which is both far smaller and far friendlier to a VR4300's 8 KB
 * data cache than a 256-way pointer table.
 *
 * ── Endianness ─────────────────────────────────────────────────────────
 * The v3 format is little-endian throughout; the N64 is big-endian. All
 * integer reads go through byte-wise helpers, so this file is endian-neutral
 * and needs no byte-swap configuration.
 *
 * ── I/O ────────────────────────────────────────────────────────────────
 * Storage is abstracted behind streamdb_emb_io_t so the same core serves
 * libdragon's DFS, a raw ROM DMA read, an SD card, or host stdio for tests.
 */
#ifndef STREAMDB_EMBEDDED_H
#define STREAMDB_EMBEDDED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Result codes. Negative values are errors. */
typedef enum {
    STREAMDB_EMB_OK = 0,
    STREAMDB_EMB_ERR_IO = -1,          /**< backend read failed / short read  */
    STREAMDB_EMB_ERR_FORMAT = -2,      /**< bad magic, version, or structure  */
    STREAMDB_EMB_ERR_CRC = -3,         /**< checksum mismatch                 */
    STREAMDB_EMB_ERR_NOMEM = -4,       /**< arena too small                   */
    STREAMDB_EMB_ERR_NOT_FOUND = -5,   /**< key absent                        */
    STREAMDB_EMB_ERR_TOO_LARGE = -6,   /**< value larger than caller's buffer */
    STREAMDB_EMB_ERR_INVAL = -7,       /**< bad argument                      */
} streamdb_emb_result_t;

/** Storage backend. `read` must fill exactly `len` bytes or fail. */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint64_t offset, void *buf, size_t len);
    uint64_t (*size)(void *ctx);
} streamdb_emb_io_t;

/** One document index entry, as stored in the index blob. */
typedef struct {
    uint8_t id[16];   /**< UUID                                   */
    uint64_t offset;  /**< byte offset of the PAYLOAD (the 8-byte
                       *   size+CRC record header precedes it)   */
    uint32_t size;    /**< payload length                         */
    uint32_t crc;     /**< CRC32 of the payload                   */
} streamdb_emb_doc_t;

/** Opaque-ish handle. Fields are exposed for stack allocation only. */
typedef struct {
    streamdb_emb_io_t io;

    /* Caller-provided arena. No malloc anywhere in this implementation:
     * on a fixed-RAM console a heap failure mid-level is not recoverable,
     * and a caller who sizes the arena up front finds out at boot instead. */
    uint8_t *arena;
    size_t arena_size;
    size_t arena_used;   /**< persistent structures, growing up   */
    size_t scratch_top;  /**< temporary blobs, growing down       */

    /* Document index, sorted by UUID (binary searchable). */
    streamdb_emb_doc_t *docs;
    uint32_t doc_count;

    /* Flat trie. Children of node i occupy nodes[i].child_first ..
     * +child_count, and their key bytes the parallel child_keys array —
     * ascending, so a child lookup is a small binary search. */
    struct streamdb_emb_node *nodes;
    uint8_t *child_keys;
    uint32_t node_count;

    /** Verify each document's CRC32 on read. Correct but not free: ~1 cycle
     *  per byte on a VR4300, so a 64 KB asset costs roughly 0.7 ms. Default
     *  on; turn it off for bulk streaming where a bad read is survivable. */
    int verify_crc;
} streamdb_emb_t;

/** Internal node layout; declared here so the struct above can be sized. */
struct streamdb_emb_node {
    uint32_t child_first;
    uint16_t child_count;
    uint8_t has_value;
    uint8_t _pad;
    uint8_t doc_id[16];
};

/**
 * Open a database. Reads and validates the newest committed header slot, then
 * loads the index and trie into `arena`.
 *
 * The arena must outlive the handle. Size it with streamdb_emb_probe().
 */
streamdb_emb_result_t streamdb_emb_open(streamdb_emb_t *db,
                                        const streamdb_emb_io_t *io,
                                        void *arena, size_t arena_size);

/**
 * Report how much arena a database needs, without loading it. Lets a caller
 * size a static buffer at build time, or fail loudly at boot rather than
 * halfway through a level load.
 */
streamdb_emb_result_t streamdb_emb_probe(const streamdb_emb_io_t *io,
                                         size_t *arena_needed);

/** Look up a key. Keys are arbitrary bytes; asset paths are the usual case. */
streamdb_emb_result_t streamdb_emb_find(const streamdb_emb_t *db,
                                        const void *key, size_t key_len,
                                        streamdb_emb_doc_t *out);

/**
 * Read a document's payload into `buf`. `*len` is the buffer size on entry and
 * the payload length on return.
 */
streamdb_emb_result_t streamdb_emb_read(const streamdb_emb_t *db,
                                        const streamdb_emb_doc_t *doc,
                                        void *buf, size_t *len);

/** Convenience: find + read in one call. */
streamdb_emb_result_t streamdb_emb_get(const streamdb_emb_t *db,
                                       const void *key, size_t key_len,
                                       void *buf, size_t *len);

/**
 * Suffix search — the reason StreamDB uses a reverse trie. Because keys are
 * indexed last-byte-first, every key ending in `suffix` shares a trie prefix,
 * so this is O(suffix_len + matches) rather than a scan.
 *
 * Calls `cb` once per match; return non-zero from `cb` to stop early. Returns
 * the number of matches visited.
 */
int streamdb_emb_find_suffix(const streamdb_emb_t *db,
                             const void *suffix, size_t suffix_len,
                             int (*cb)(const streamdb_emb_doc_t *doc, void *user),
                             void *user);

/** Number of documents in the database. */
static inline uint32_t streamdb_emb_count(const streamdb_emb_t *db)
{
    return db ? db->doc_count : 0u;
}

/** Human-readable result string, for debugf/asserts. */
const char *streamdb_emb_strerror(streamdb_emb_result_t r);

/* ── Backends ────────────────────────────────────────────────────────── */

#ifdef STREAMDB_EMB_BACKEND_STDIO
/** Host backend, for tests and tooling. `path` must outlive the handle. */
streamdb_emb_result_t streamdb_emb_io_stdio(streamdb_emb_io_t *io,
                                            void *storage, const char *path);
void streamdb_emb_io_stdio_close(streamdb_emb_io_t *io);
/** Bytes of `storage` the stdio backend needs. */
size_t streamdb_emb_io_stdio_size(void);
#endif

#ifdef STREAMDB_EMB_BACKEND_DFS
/** libdragon DFS backend: reads a file out of the ROM filesystem. */
streamdb_emb_result_t streamdb_emb_io_dfs(streamdb_emb_io_t *io,
                                          void *storage, const char *path);
void streamdb_emb_io_dfs_close(streamdb_emb_io_t *io);
size_t streamdb_emb_io_dfs_size(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* STREAMDB_EMBEDDED_H */
