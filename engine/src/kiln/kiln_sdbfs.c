/* SPDX-License-Identifier: MIT
 *
 * kiln_sdbfs.c — see kiln_sdbfs.h for why this exists and what it replaces.
 */

#include "kiln_sdbfs.h"

#include <libdragon.h>
#include <dragonfs.h>   /* IODFS_GET_ROM_BASE */
/* <libdragon.h> pulls in dragonfs.h but NOT system.h, so filesystem_t and
 * attach_filesystem have to be asked for by name. Every other kiln_ module
 * gets away with the umbrella header because none of them registers a
 * filesystem. */
#include <system.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int                 used;
    streamdb_emb_doc_t  doc;
    uint64_t            pos;
} SdbFile;

static KilnAsset *g_db;
static char       g_prefix[16];
static int        g_mounted;
static SdbFile    g_files[KILN_SDBFS_MAX_OPEN];

/* ── Helpers ─────────────────────────────────────────────────────────── */

static SdbFile *slot_of(void *file)
{
    /* Handles are biased by one so that slot 0 is not the NULL that `open`
     * uses for failure. The same off-by-one kiln_cache's handle packing was
     * bitten by once; not reintroduced here. */
    uintptr_t i = (uintptr_t)file;
    if (i == 0 || i > KILN_SDBFS_MAX_OPEN) return NULL;
    SdbFile *f = &g_files[i - 1];
    return f->used ? f : NULL;
}

static int lookup(const char *name, streamdb_emb_doc_t *out)
{
    streamdb_emb_t *r = kiln_asset_reader(g_db);
    if (!r || !name) return -1;
    return streamdb_emb_find(r, name, strlen(name), out) == STREAMDB_EMB_OK
               ? 0 : -1;
}

/* ── filesystem_t hooks ──────────────────────────────────────────────── */

static void *sdbfs_open(char *name, int flags)
{
    if (!g_mounted) { errno = ENODEV; return NULL; }

    /* Read-only, same refusal DragonFS gives (dragonfs.c:1119). A StreamDB
     * in ROM is not writable in any sense worth pretending about. */
    if ((flags & O_ACCMODE) != O_RDONLY) { errno = EACCES; return NULL; }

    /* `name` arrives with the mount prefix already stripped by system.c:1042,
     * which makes it exactly the key mkStreamdb wrote. A leading slash is
     * tolerated because a caller writing "sdb://music/x" is making a typo
     * that costs nothing to absorb, not expressing a different path. */
    if (name[0] == '/') name++;

    streamdb_emb_doc_t doc;
    if (lookup(name, &doc) != 0) { errno = ENOENT; return NULL; }

    for (int i = 0; i < KILN_SDBFS_MAX_OPEN; i++) {
        if (g_files[i].used) continue;
        g_files[i].used = 1;
        g_files[i].doc  = doc;
        g_files[i].pos  = 0;
        return (void *)(uintptr_t)(i + 1);
    }
    errno = EMFILE;
    return NULL;
}

static int sdbfs_close(void *file)
{
    SdbFile *f = slot_of(file);
    if (!f) { errno = EBADF; return -1; }
    f->used = 0;
    return 0;
}

static int sdbfs_read(void *file, uint8_t *ptr, int len)
{
    SdbFile *f = slot_of(file);
    if (!f) { errno = EBADF; return -1; }
    if (len <= 0) return 0;

    streamdb_emb_t *r = kiln_asset_reader(g_db);
    if (!r) { errno = ENODEV; return -1; }

    size_t n = (size_t)len;
    streamdb_emb_result_t rr = streamdb_emb_read_range(r, &f->doc, f->pos,
                                                       ptr, &n);
    if (rr != STREAMDB_EMB_OK) { errno = EIO; return -1; }

    f->pos += n;
    return (int)n;
}

static int sdbfs_lseek(void *file, int ptr, int dir)
{
    SdbFile *f = slot_of(file);
    if (!f) { errno = EBADF; return -1; }

    int64_t base;
    switch (dir) {
        case SEEK_SET: base = 0;                   break;
        case SEEK_CUR: base = (int64_t)f->pos;     break;
        case SEEK_END: base = (int64_t)f->doc.size; break;
        default: errno = EINVAL; return -1;
    }

    int64_t want = base + (int64_t)ptr;
    /* Seeking TO the end is legal and normal — it is how a caller asks how
     * big a file is. Seeking past it is not: this is a read-only container
     * with no hole to create, so a caller doing it has miscalculated and
     * should hear about it now rather than at the read. */
    if (want < 0 || want > (int64_t)f->doc.size) { errno = EINVAL; return -1; }

    f->pos = (uint64_t)want;
    return (int)want;
}

static void fill_stat(const streamdb_emb_doc_t *doc, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_size = (off_t)doc->size;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
}

static int sdbfs_fstat(void *file, struct stat *st)
{
    SdbFile *f = slot_of(file);
    if (!f || !st) { errno = EBADF; return -1; }
    fill_stat(&f->doc, st);
    return 0;
}

static int sdbfs_stat(char *name, struct stat *st)
{
    if (!g_mounted || !st) { errno = ENODEV; return -1; }
    if (name[0] == '/') name++;

    streamdb_emb_doc_t doc;
    if (lookup(name, &doc) != 0) { errno = ENOENT; return -1; }
    fill_stat(&doc, st);
    return 0;
}

static int sdbfs_ioctl(void *file, unsigned long cmd, void *argp)
{
    /* THIS IS THE WHOLE POINT OF THE FILE. wav64.c:214, asset.c:171 and
     * ringbuf.c:121 all ask this one question, and answering it is what buys
     * the cartridge DMA path for every libdragon loader at once. */
    if (cmd != IODFS_GET_ROM_BASE) { errno = ENOTTY; return -1; }

    SdbFile *f = slot_of(file);
    if (!f || !argp) { errno = EBADF; return -1; }

    streamdb_emb_t *r = kiln_asset_reader(g_db);
    if (!r) { errno = ENODEV; return -1; }

    uint32_t base = 0;
    if (streamdb_emb_doc_rom_base(r, &f->doc, &base) != STREAMDB_EMB_OK) {
        errno = EIO;
        return -1;
    }

    /* Zero is a legitimate answer meaning "no address, use read()", and
     * every caller already branches on it (wav64.c:93 sets async_read from
     * exactly this). Reporting success with zero is therefore correct and an
     * error would be wrong — an error makes a working copy path look broken. */
    *(uint32_t *)argp = base;
    return 0;
}

static filesystem_t sdb_fs = {
    /* Read-only with no shared mutable state beyond the open table, and the
     * open table is only touched by open/close. Marked thread safe on the
     * same reasoning system.h gives: "read-only filesystems are easily thread
     * safe: only pay attention to some shared mutable state like eg some
     * global cache." There is none here. */
    .thread_safe = true,
    .open  = sdbfs_open,
    .fstat = sdbfs_fstat,
    .stat  = sdbfs_stat,
    .lseek = sdbfs_lseek,
    .read  = sdbfs_read,
    .write = NULL,       /* ROM. DragonFS leaves this NULL too. */
    .close = sdbfs_close,
    .ioctl = sdbfs_ioctl,
};

/* ── Public ──────────────────────────────────────────────────────────── */

int kiln_sdbfs_mount(const char *prefix, KilnAsset *db)
{
    if (g_mounted || !prefix || !db) return -1;
    if (!kiln_asset_reader(db)) return -1;

    size_t n = strlen(prefix);
    if (n < 3 || n >= sizeof g_prefix) return -1;
    if (prefix[n - 1] != '/' || prefix[n - 2] != ':') return -1;

    g_db = db;
    memset(g_files, 0, sizeof g_files);
    memcpy(g_prefix, prefix, n + 1);

    if (attach_filesystem(g_prefix, &sdb_fs) != 0) {
        g_db = NULL;
        g_prefix[0] = 0;
        return -1;
    }
    g_mounted = 1;
    return 0;
}

void kiln_sdbfs_unmount(void)
{
    if (!g_mounted) return;
    detach_filesystem(g_prefix);
    memset(g_files, 0, sizeof g_files);
    g_prefix[0] = 0;
    g_db = NULL;
    g_mounted = 0;
}

int kiln_sdbfs_key_is_dma(const char *key, size_t key_len)
{
    streamdb_emb_t *r = kiln_asset_reader(g_db);
    if (!r || !key || !key_len) return 0;

    streamdb_emb_doc_t doc;
    if (streamdb_emb_find(r, key, key_len, &doc) != STREAMDB_EMB_OK) return 0;

    uint32_t base = 0;
    if (streamdb_emb_doc_rom_base(r, &doc, &base) != STREAMDB_EMB_OK) return 0;

    /* Even address only. libdragon's own test is a parity comparison against
     * the destination (wav64_vadpcm.c:242); a destination buffer is always at
     * least 2-byte aligned, so an even source is what makes that test pass. */
    return base != 0 && (base & 1u) == 0;
}
