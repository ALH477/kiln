/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * streamdb_embedded.c — StreamDB v3 reader for bare-metal targets.
 * See streamdb_embedded.h for the rationale and the memory strategy.
 *
 * Format (from the upstream C edition's header comment, v3):
 *
 *   [0..128)   header slot 0        [128..256) header slot 1
 *   [256..)    documents, then appended trie/index commit blobs
 *
 *   Header slot (128 B, 76 used, little-endian):
 *     0..4  "STDB"        4..8   version u32 (=3)
 *     8..16 commit seq    16..24 trie offset   24..32 trie len
 *     32..36 trie CRC32   40..48 index offset  48..56 index len
 *     56..60 index CRC32  64..72 data end      72..76 CRC32 of [0..72)
 *
 *   Document record: u32 size + u32 CRC32(payload) + payload
 *   Index blob:      u64 count + (16B UUID, u64 offset, u32 size, u32 crc)*
 *                    sorted by UUID byte order
 *   Trie node:       u64 nchild + (u8 key, node)* + tag + u64 count
 *                    tag: 0 = no value; 1 = u64 len(=16) + 16B UUID
 *                    NOTE children precede the value tag, ascending by key.
 */

#include "streamdb_embedded.h"

#include <string.h>

#define HEADER_SLOT_SIZE 128u
#define HEADER_SLOTS 2u
#define DATA_START 256u
#define STREAMDB_MAGIC "STDB"
#define STREAMDB_FORMAT_VERSION 3u
#define MAX_KEY_LEN 1024u

/* ── CRC32 (IEEE 802.3, reflected — identical to zlib and crc32fast) ──
 * Computed on the fly rather than from a 1 KB table: on a machine with an
 * 8 KB data cache, a table this size competes with the data being hashed,
 * and CRC is not on any hot path here. */
static uint32_t crc32_bytes(const uint8_t *p, size_t len, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return ~crc;
}

/* ── Little-endian readers. Byte-wise, so alignment- and endian-neutral. ── */
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

/* ── Arena ────────────────────────────────────────────────────────────
 * Two allocators over one buffer. Persistent structures (index, nodes,
 * child keys) grow up from the bottom; the trie blob is scratch — needed
 * only while parsing — and is taken from the top, then released.
 *
 * That split matters: the blob is often the single largest allocation (60 KB
 * of a 135 KB arena for a 200-asset database), and holding it for the life of
 * the handle would waste more memory than the parsed index costs.
 */
static void *arena_alloc(streamdb_emb_t *db, size_t n)
{
    /* 8-byte align: the doc index is read as u64s. */
    size_t base = (db->arena_used + 7u) & ~(size_t)7u;
    if (base + n > db->scratch_top) return NULL;
    db->arena_used = base + n;
    return db->arena + base;
}

/* Scratch, from the top. Freed wholesale by restoring scratch_top. */
static void *arena_scratch(streamdb_emb_t *db, size_t n)
{
    size_t top = db->scratch_top;
    if (n > top) return NULL;
    size_t base = (top - n) & ~(size_t)7u;
    if (base < db->arena_used) return NULL;
    db->scratch_top = base;
    return db->arena + base;
}

/* ── Header ─────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t seq, trie_off, trie_len, index_off, index_len, data_end;
    uint32_t trie_crc, index_crc;
} header_t;

static int header_parse(const uint8_t *buf, uint64_t file_len, header_t *h)
{
    if (memcmp(buf, STREAMDB_MAGIC, 4) != 0) return 0;
    if (rd_u32(buf + 4) != STREAMDB_FORMAT_VERSION) return 0;

    /* The slot's own CRC covers bytes 0..72. A slot that fails this was
     * torn mid-write; the caller falls back to the other slot. */
    if (rd_u32(buf + 72) != crc32_bytes(buf, 72, 0)) return 0;

    h->seq       = rd_u64(buf + 8);
    h->trie_off  = rd_u64(buf + 16);
    h->trie_len  = rd_u64(buf + 24);
    h->trie_crc  = rd_u32(buf + 32);
    h->index_off = rd_u64(buf + 40);
    h->index_len = rd_u64(buf + 48);
    h->index_crc = rd_u32(buf + 56);
    h->data_end  = rd_u64(buf + 64);

    /* Bounds: a valid commit never points outside the file. */
    if (h->trie_off  + h->trie_len  > file_len) return 0;
    if (h->index_off + h->index_len > file_len) return 0;
    if (h->data_end > file_len) return 0;
    if (h->index_len < 8u) return 0;

    return 1;
}

/* ── Trie parsing ────────────────────────────────────────────────────
 * Two passes over the blob. The first counts nodes so the arena can be
 * carved exactly; the second fills the flat array. Recursion depth is
 * bounded by MAX_KEY_LEN, and both passes reject anything inconsistent.
 */
typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} cur_t;

static int cur_bytes(cur_t *c, size_t n, const uint8_t **out)
{
    if (c->pos + n > c->len) return 0;
    if (out) *out = c->p + c->pos;
    c->pos += n;
    return 1;
}

static int cur_u64(cur_t *c, uint64_t *v)
{
    const uint8_t *p;
    if (!cur_bytes(c, 8, &p)) return 0;
    *v = rd_u64(p);
    return 1;
}

/* Pass 1: how many nodes does this blob describe? */
static int trie_count(cur_t *c, unsigned depth, uint32_t *count)
{
    if (depth > MAX_KEY_LEN) return 0;

    uint64_t nchild;
    if (!cur_u64(c, &nchild)) return 0;
    if (nchild > 256u) return 0; /* keys are single bytes */

    (*count)++;

    for (uint64_t i = 0; i < nchild; i++) {
        if (!cur_bytes(c, 1, NULL)) return 0;          /* key byte */
        if (!trie_count(c, depth + 1, count)) return 0; /* child    */
    }

    const uint8_t *tag;
    if (!cur_bytes(c, 1, &tag)) return 0;
    if (*tag == 1u) {
        uint64_t ulen;
        if (!cur_u64(c, &ulen) || ulen != 16u) return 0;
        if (!cur_bytes(c, 16, NULL)) return 0;
    } else if (*tag != 0u) {
        return 0;
    }

    uint64_t subtree;
    if (!cur_u64(c, &subtree)) return 0;
    return 1;
}

/* Pass 2: materialise into `slot`.
 *
 * Each node fills a slot its parent already assigned it, and bump-allocates a
 * fresh contiguous range for its OWN children before recursing. Writing into
 * an assigned slot rather than returning a freshly allocated one is what keeps
 * siblings adjacent: a node's children occupy consecutive indices even though
 * their subtrees allocate arbitrarily far ahead. That adjacency is the whole
 * point — it lets node_child() binary-search and keeps a descent touching few
 * cache lines.
 *
 * Returns 0 on success, non-zero on malformed input. */
static int trie_build(cur_t *c, streamdb_emb_t *db, uint32_t slot,
                      uint32_t *bump, unsigned depth)
{
    if (depth > MAX_KEY_LEN) return -1;
    if (slot >= db->node_count) return -1;

    uint64_t nchild;
    if (!cur_u64(c, &nchild) || nchild > 256u) return -1;

    struct streamdb_emb_node *node = &db->nodes[slot];
    memset(node, 0, sizeof(*node));

    /* Children are serialised inline, ascending by key byte. */
    if (*bump + nchild > db->node_count) return -1;
    uint32_t first = *bump;
    *bump += (uint32_t)nchild;

    node->child_first = first;
    node->child_count = (uint16_t)nchild;

    for (uint64_t i = 0; i < nchild; i++) {
        const uint8_t *key;
        if (!cur_bytes(c, 1, &key)) return -1;
        db->child_keys[first + i] = *key;
        if (trie_build(c, db, first + (uint32_t)i, bump, depth + 1) != 0) {
            return -1;
        }
        /* `node` may not be reused across the recursion without re-deriving
         * it: db->nodes is stable here, but keep the read local anyway. */
        node = &db->nodes[slot];
    }

    const uint8_t *tag;
    if (!cur_bytes(c, 1, &tag)) return -1;
    if (*tag == 1u) {
        uint64_t ulen;
        const uint8_t *id;
        if (!cur_u64(c, &ulen) || ulen != 16u) return -1;
        if (!cur_bytes(c, 16, &id)) return -1;
        memcpy(node->doc_id, id, 16);
        node->has_value = 1u;
    } else if (*tag != 0u) {
        return -1;
    }

    uint64_t subtree;
    if (!cur_u64(c, &subtree)) return -1;

    return 0;
}

/* ── Index ──────────────────────────────────────────────────────────── */
static int doc_cmp(const uint8_t a[16], const uint8_t b[16])
{
    return memcmp(a, b, 16);
}

static const streamdb_emb_doc_t *index_find(const streamdb_emb_t *db,
                                            const uint8_t id[16])
{
    /* Entries are sorted by UUID byte order, per the format. */
    uint32_t lo = 0, hi = db->doc_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        int c = doc_cmp(db->docs[mid].id, id);
        if (c == 0) return &db->docs[mid];
        if (c < 0) lo = mid + 1u; else hi = mid;
    }
    return NULL;
}

/* ── Open ───────────────────────────────────────────────────────────── */
static streamdb_emb_result_t read_commit(const streamdb_emb_io_t *io,
                                         header_t *out, uint64_t *file_len_out)
{
    if (!io || !io->read || !io->size) return STREAMDB_EMB_ERR_INVAL;

    uint64_t file_len = io->size(io->ctx);
    if (file_len < DATA_START) return STREAMDB_EMB_ERR_FORMAT;

    uint8_t slot[HEADER_SLOT_SIZE];
    header_t best;
    int have = 0;

    /* Two alternating slots; the newest one that validates wins. This is the
     * torn-write fallback the upstream durability protocol is built around. */
    for (unsigned i = 0; i < HEADER_SLOTS; i++) {
        if (io->read(io->ctx, (uint64_t)i * HEADER_SLOT_SIZE, slot,
                     HEADER_SLOT_SIZE) != 0) {
            return STREAMDB_EMB_ERR_IO;
        }
        header_t h;
        if (!header_parse(slot, file_len, &h)) continue;
        if (!have || h.seq > best.seq) { best = h; have = 1; }
    }

    if (!have) return STREAMDB_EMB_ERR_FORMAT;
    *out = best;
    if (file_len_out) *file_len_out = file_len;
    return STREAMDB_EMB_OK;
}

streamdb_emb_result_t streamdb_emb_probe(const streamdb_emb_io_t *io,
                                         size_t *arena_needed)
{
    if (!arena_needed) return STREAMDB_EMB_ERR_INVAL;

    header_t h;
    streamdb_emb_result_t r = read_commit(io, &h, NULL);
    if (r != STREAMDB_EMB_OK) return r;

    /* Documents: u64 count then 32 bytes each. */
    uint8_t cnt[8];
    if (io->read(io->ctx, h.index_off, cnt, 8) != 0) return STREAMDB_EMB_ERR_IO;
    uint64_t n = rd_u64(cnt);
    if (n > 0xFFFFFFFFull) return STREAMDB_EMB_ERR_FORMAT;
    if (8u + n * 32u > h.index_len) return STREAMDB_EMB_ERR_FORMAT;

    /* The trie needs a pass to count nodes, so the blob has to be read; do it
     * into the caller's future arena is not possible here, so estimate from
     * the blob length instead. A node is at minimum 10 bytes on the wire
     * (u64 nchild + u8 tag + u64 count is 17, so this is conservative). */
    uint64_t max_nodes = h.trie_len / 10u + 1u;

    size_t need = (size_t)(n * sizeof(streamdb_emb_doc_t))
                + (size_t)(max_nodes * sizeof(struct streamdb_emb_node))
                + (size_t)max_nodes
                + (size_t)h.trie_len   /* the blob itself, during parse */
                + 64u;                 /* alignment slack */
    *arena_needed = need;
    return STREAMDB_EMB_OK;
}

streamdb_emb_result_t streamdb_emb_open(streamdb_emb_t *db,
                                        const streamdb_emb_io_t *io,
                                        void *arena, size_t arena_size)
{
    if (!db || !io || !arena) return STREAMDB_EMB_ERR_INVAL;

    memset(db, 0, sizeof(*db));
    db->io = *io;
    db->arena = (uint8_t *)arena;
    db->arena_size = arena_size;
    db->scratch_top = arena_size;
    db->verify_crc = 1;

    header_t h;
    uint64_t file_len;
    streamdb_emb_result_t r = read_commit(io, &h, &file_len);
    if (r != STREAMDB_EMB_OK) return r;

    /* ── index ── */
    size_t scratch_mark = db->scratch_top;
    uint8_t *idx = arena_scratch(db, (size_t)h.index_len);
    if (!idx) return STREAMDB_EMB_ERR_NOMEM;
    if (io->read(io->ctx, h.index_off, idx, (size_t)h.index_len) != 0) {
        return STREAMDB_EMB_ERR_IO;
    }
    if (crc32_bytes(idx, (size_t)h.index_len, 0) != h.index_crc) {
        return STREAMDB_EMB_ERR_CRC;
    }

    uint64_t n = rd_u64(idx);
    if (n > 0xFFFFFFFFull || 8u + n * 32u > h.index_len) {
        return STREAMDB_EMB_ERR_FORMAT;
    }
    db->doc_count = (uint32_t)n;
    db->docs = (streamdb_emb_doc_t *)arena_alloc(
        db, (size_t)n * sizeof(streamdb_emb_doc_t));
    if (!db->docs && n) return STREAMDB_EMB_ERR_NOMEM;

    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *e = idx + 8u + i * 32u;
        memcpy(db->docs[i].id, e, 16);
        db->docs[i].offset = rd_u64(e + 16);
        db->docs[i].size   = rd_u32(e + 24);
        db->docs[i].crc    = rd_u32(e + 28);
        /* offset addresses the payload; the 8-byte record header precedes it. */
        if (db->docs[i].offset < 8u ||
            db->docs[i].offset + db->docs[i].size > file_len) {
            return STREAMDB_EMB_ERR_FORMAT;
        }
    }

    /* ── trie ── */
    uint8_t *blob = arena_scratch(db, (size_t)h.trie_len);
    if (!blob && h.trie_len) return STREAMDB_EMB_ERR_NOMEM;
    if (h.trie_len) {
        if (io->read(io->ctx, h.trie_off, blob, (size_t)h.trie_len) != 0) {
            return STREAMDB_EMB_ERR_IO;
        }
        if (crc32_bytes(blob, (size_t)h.trie_len, 0) != h.trie_crc) {
            return STREAMDB_EMB_ERR_CRC;
        }

        cur_t c = { blob, (size_t)h.trie_len, 0 };
        uint32_t count = 0;
        if (!trie_count(&c, 0, &count)) return STREAMDB_EMB_ERR_FORMAT;

        db->nodes = (struct streamdb_emb_node *)arena_alloc(
            db, (size_t)count * sizeof(struct streamdb_emb_node));
        db->child_keys = (uint8_t *)arena_alloc(db, count);
        if (!db->nodes || !db->child_keys) return STREAMDB_EMB_ERR_NOMEM;
        db->node_count = count;

        c.pos = 0;
        uint32_t bump = 1u; /* slot 0 is the root */
        if (trie_build(&c, db, 0u, &bump, 0) != 0) {
            return STREAMDB_EMB_ERR_FORMAT;
        }
    }

    /* Both blobs are now fully parsed into the persistent structures below
     * arena_used, so the scratch region goes back to the caller. */
    db->scratch_top = scratch_mark;
    return STREAMDB_EMB_OK;
}

/* ── Lookup ─────────────────────────────────────────────────────────── */
static uint32_t node_child(const streamdb_emb_t *db, uint32_t node, uint8_t key)
{
    const struct streamdb_emb_node *n = &db->nodes[node];
    uint32_t lo = 0, hi = n->child_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        uint8_t k = db->child_keys[n->child_first + mid];
        if (k == key) return n->child_first + mid;
        if (k < key) lo = mid + 1u; else hi = mid;
    }
    return UINT32_MAX;
}

/* Walk the reverse trie: keys are indexed last byte first. */
static uint32_t trie_walk(const streamdb_emb_t *db,
                          const uint8_t *key, size_t key_len)
{
    if (!db->node_count) return UINT32_MAX;
    uint32_t cur = 0;
    for (size_t i = key_len; i-- > 0;) {
        cur = node_child(db, cur, key[i]);
        if (cur == UINT32_MAX) return UINT32_MAX;
    }
    return cur;
}

streamdb_emb_result_t streamdb_emb_find(const streamdb_emb_t *db,
                                        const void *key, size_t key_len,
                                        streamdb_emb_doc_t *out)
{
    if (!db || !key || !key_len || key_len > MAX_KEY_LEN) {
        return STREAMDB_EMB_ERR_INVAL;
    }

    uint32_t node = trie_walk(db, (const uint8_t *)key, key_len);
    if (node == UINT32_MAX || !db->nodes[node].has_value) {
        return STREAMDB_EMB_ERR_NOT_FOUND;
    }

    const streamdb_emb_doc_t *doc = index_find(db, db->nodes[node].doc_id);
    if (!doc) return STREAMDB_EMB_ERR_NOT_FOUND;
    if (out) *out = *doc;
    return STREAMDB_EMB_OK;
}

streamdb_emb_result_t streamdb_emb_read(const streamdb_emb_t *db,
                                        const streamdb_emb_doc_t *doc,
                                        void *buf, size_t *len)
{
    if (!db || !doc || !buf || !len) return STREAMDB_EMB_ERR_INVAL;
    if (*len < doc->size) { *len = doc->size; return STREAMDB_EMB_ERR_TOO_LARGE; }

    /* The index entry's offset points at the PAYLOAD, not at the record
     * header — upstream writes `e.offset = next_offset + 8` after emitting the
     * 8-byte header. The format comment ("Document record: size u32 + CRC32
     * u32 + payload") does not spell that out, and reading it the obvious way
     * yields a size mismatch on every document. The header therefore lives at
     * offset-8, and is used only as a consistency check.
     */
    if (doc->offset >= 8u) {
        uint8_t hdr[8];
        if (db->io.read(db->io.ctx, doc->offset - 8u, hdr, 8) != 0) {
            return STREAMDB_EMB_ERR_IO;
        }
        if (rd_u32(hdr) != doc->size) return STREAMDB_EMB_ERR_FORMAT;
        if (rd_u32(hdr + 4) != doc->crc) return STREAMDB_EMB_ERR_FORMAT;
    }

    if (db->io.read(db->io.ctx, doc->offset, buf, doc->size) != 0) {
        return STREAMDB_EMB_ERR_IO;
    }

    if (db->verify_crc) {
        if (crc32_bytes((const uint8_t *)buf, doc->size, 0) != doc->crc) {
            return STREAMDB_EMB_ERR_CRC;
        }
    }

    *len = doc->size;
    return STREAMDB_EMB_OK;
}

streamdb_emb_result_t streamdb_emb_get(const streamdb_emb_t *db,
                                       const void *key, size_t key_len,
                                       void *buf, size_t *len)
{
    streamdb_emb_doc_t doc;
    streamdb_emb_result_t r = streamdb_emb_find(db, key, key_len, &doc);
    if (r != STREAMDB_EMB_OK) return r;
    return streamdb_emb_read(db, &doc, buf, len);
}

/* ── Suffix search ──────────────────────────────────────────────────── */
typedef struct {
    const streamdb_emb_t *db;
    int (*cb)(const streamdb_emb_doc_t *, void *);
    void *user;
    int count;
    int stop;
} collect_t;

static void collect(collect_t *st, uint32_t node)
{
    if (st->stop) return;

    const struct streamdb_emb_node *n = &st->db->nodes[node];
    if (n->has_value) {
        const streamdb_emb_doc_t *doc = index_find(st->db, n->doc_id);
        if (doc) {
            st->count++;
            if (st->cb && st->cb(doc, st->user)) { st->stop = 1; return; }
        }
    }
    for (uint16_t i = 0; i < n->child_count; i++) {
        collect(st, n->child_first + i);
        if (st->stop) return;
    }
}

int streamdb_emb_find_suffix(const streamdb_emb_t *db,
                             const void *suffix, size_t suffix_len,
                             int (*cb)(const streamdb_emb_doc_t *, void *),
                             void *user)
{
    if (!db || !suffix || !suffix_len || suffix_len > MAX_KEY_LEN) return 0;

    /* Every key ending in `suffix` shares the trie path spelled by that
     * suffix reversed — this is exactly what the reverse trie buys. */
    uint32_t node = trie_walk(db, (const uint8_t *)suffix, suffix_len);
    if (node == UINT32_MAX) return 0;

    collect_t st = { db, cb, user, 0, 0 };
    collect(&st, node);
    return st.count;
}

const char *streamdb_emb_strerror(streamdb_emb_result_t r)
{
    switch (r) {
        case STREAMDB_EMB_OK:            return "ok";
        case STREAMDB_EMB_ERR_IO:        return "I/O error";
        case STREAMDB_EMB_ERR_FORMAT:    return "bad format";
        case STREAMDB_EMB_ERR_CRC:       return "checksum mismatch";
        case STREAMDB_EMB_ERR_NOMEM:     return "arena too small";
        case STREAMDB_EMB_ERR_NOT_FOUND: return "not found";
        case STREAMDB_EMB_ERR_TOO_LARGE: return "buffer too small";
        case STREAMDB_EMB_ERR_INVAL:     return "invalid argument";
    }
    return "unknown";
}
