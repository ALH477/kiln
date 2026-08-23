/* SPDX-License-Identifier: MIT
 *
 * kiln_store.c — see kiln_store.h for what this is and why it exists.
 *
 * Implementation notes worth keeping next to the code:
 *
 * - **cart_init() is called explicitly**, before debug_init_sdfs(). libdragon
 *   mounts the SD volume DEFERRED, so debug_init_sdfs() can return true without
 *   having touched the card at all; the first real failure would then surface
 *   from an fopen several screens later. Probing the cart up front means
 *   kiln_store_cart_name() has something true to report even when the mount is
 *   the thing that failed, which is the difference between "no cart" and "cart,
 *   no card".
 *
 * - **The mount is verified with a real open**, not with the mount's return
 *   value, for the same reason.
 *
 * - **Bus mode is inferred from measured throughput, not read from a
 *   register.** libcart keeps its SPI-vs-4-bit-SD decision in a file-static
 *   (`__sd_type` in cart.c), and the register that decides it lives behind
 *   libcart's own PI timing save/restore. Reading it from outside would mean
 *   poking DOM2 while another subsystem believes it owns the timing — and a
 *   wrong guess there hangs the console, which on a cart with no USB is
 *   indistinguishable from a bad ROM. So the probe times a 64 KB write+read
 *   instead and classifies. A number that was measured is also more useful than
 *   a flag that was read.
 *
 * - **The SRAM backend keeps its directory in the first 128 bytes** and never
 *   moves a payload once written: slots are fixed-size and fixed-offset. A
 *   compacting allocator over a 32 KB save chip would be a lot of code whose
 *   failure mode is losing the level it was compacting.
 */
#include <libdragon.h>
#include <libcart/cart.h>
#include <sram.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   /* mkdir — newlib's, not libdragon's */

#include "kiln_store.h"

/* ── Module state ──────────────────────────────────────────────────────*/

static KilnStoreKind g_kind = KILN_STORE_NONE;
static int          g_cart = CART_NULL;
static int          g_inited;
static int          g_sram_ok;
static const char  *g_bus = "-";

/* SRAM image layout. The directory is deliberately dumb: fixed slots, fixed
 * payload offsets, no free list. `used` doubles as the valid flag so a wiped
 * (all-zero) SRAM parses as an empty directory rather than as garbage.
 */
#define SRAM_DIR_BYTES  (KILN_STORE_SRAM_SLOTS * 32)
/* The top 8 bytes are reserved as try_sram's round-trip scratch — deliberately
 * ABOVE every slot, so a probe that runs at boot on a cart whose save chip is
 * flaky cannot land inside a level someone saved. */
#define SRAM_PROBE_BYTES 8
#define SRAM_SLOT_BYTES ((KILN_STORE_SRAM_BYTES - SRAM_DIR_BYTES - SRAM_PROBE_BYTES) \
                         / KILN_STORE_SRAM_SLOTS)

typedef struct {
    char     name[KILN_STORE_NAME_MAX];
    uint32_t used;   /* payload+header bytes in this slot, 0 = free */
    uint32_t _pad;
} SramEntry;

/* ── CRC-32 ────────────────────────────────────────────────────────────
 *
 * Bitwise rather than table-driven: a 1 KB table in an engine that budgets for
 * 4 MB RDRAM, to checksum a payload written at most once per save, is the wrong
 * trade. 8 shifts per byte on a 93.75 MHz VR4300 is ~2 ms for a 64 KB level,
 * which is invisible next to the SD write it protects.
 */
uint32_t kiln_store_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

/* ── Names ─────────────────────────────────────────────────────────────*/

const char *kiln_store_kind_name(void)
{
    switch (g_kind) {
    case KILN_STORE_CART_SD: return "sd";
    case KILN_STORE_SRAM:    return "sram";
    case KILN_STORE_DFS:     return "rom";
    default:                return "none";
    }
}

const char *kiln_store_cart_name(void)
{
    switch (g_cart) {
    case CART_CI:  return "64drive";
    case CART_EDX: return "ED64-X";
    case CART_ED:  return "ED64";     /* V1/V2/V2.5/V3/ED64+ share a driver */
    case CART_SC:  return "SC64";
    default:       return "none";
    }
}

const char *kiln_store_bus_name(void) { return g_bus; }

const char *kiln_store_status_name(int status)
{
    switch (status) {
    case KILN_STORE_OK:        return "ok";
    case KILN_STORE_ENOINIT:   return "no-init";
    case KILN_STORE_EREADONLY: return "read-only";
    case KILN_STORE_EOPEN:     return "open-failed";
    case KILN_STORE_EIO:       return "short-io";
    case KILN_STORE_ENOSPACE:  return "no-space";
    case KILN_STORE_ENOENT:    return "not-found";
    case KILN_STORE_EMAGIC:    return "bad-magic";
    case KILN_STORE_EVERSION:  return "bad-version";
    case KILN_STORE_ECRC:      return "bad-crc";
    case KILN_STORE_ENAME:     return "bad-name";
    default:                  return "unknown";
    }
}

/* ── Name validation ───────────────────────────────────────────────────
 *
 * A bare name only. This repo lost a whole screen to an asset whose `name` was
 * treated as a label when it was in fact the filename, so a name carrying a
 * separator or an extension is refused rather than quietly reinterpreted.
 */
static int name_ok(const char *name)
{
    if (!name || !name[0]) return 0;
    size_t n = strlen(name);
    if (n >= KILN_STORE_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z');
        if (!alnum && c != '_' && c != '-') return 0;
    }
    return 1;
}

static void path_for(char *out, size_t cap, const char *name, const char *ext)
{
    snprintf(out, cap, "%s/%s.%s", KILN_STORE_DIR, name, ext);
}

/* ── Lifecycle ─────────────────────────────────────────────────────────*/

static int try_sd(void)
{
    /* cart_init() returns >= 0 with cart_type set, or -1. It is also the only
     * thing that establishes the PI DOM1/DOM2 timing the card driver needs. */
    if (cart_init() < 0) return 0;
    g_cart = cart_type;

    if (!debug_init_sdfs("sd:/", -1)) return 0;

    /* The mount is deferred, so prove the card by touching it. mkdir first:
     * on a fresh card FORGE/ does not exist yet, and a failed open of a file
     * inside a missing directory is not evidence about the card. */
    mkdir(KILN_STORE_DIR, 0777);

    char path[64];
    path_for(path, sizeof path, "MOUNT", "TMP");
    FILE *f = fopen(path, "wb");
    if (!f) { debug_close_sdfs(); return 0; }
    int wrote = (fputs("kiln\n", f) >= 0);
    fclose(f);
    remove(path);
    if (!wrote) { debug_close_sdfs(); return 0; }
    return 1;
}

/* ── Why this is not just `if (sram_detect() < 0) return 0;` ───────────
 *
 * Because that is what it was, and the probe caught it on its first run: under
 * Ares, with no save type declared in the ROM header, the write reported OK and
 * the read back reported not-found.
 *
 * `sram.h` documents sram_detect() as returning "Size of available SRAM if is
 * detected, -1 otherwise". `src/sram.c` returns `is_detected ? SRAM_SIZE : 0`.
 * It never returns a negative, so `< 0` is a test that cannot fail, and the
 * SRAM backend was selected on every machine including ones with no save chip
 * at all — after which every write silently went nowhere and every read came
 * back as zeros, which parses as a valid EMPTY directory. Exactly the shape of
 * failure this repo keeps paying for: each layer behaved reasonably and the
 * composition lost the data in silence.
 *
 * So: require a POSITIVE size, and then do not take even that on trust. A
 * four-byte round trip through a scratch word at the very top of the region
 * (above every slot, so a failed probe cannot corrupt a saved level) is two DMA
 * transfers and settles the question for the actual hardware in front of us
 * rather than for the hardware the header describes.
 */
/* ── The read-only ROM backend has to mount its own filesystem ──────────
 *
 * `dfs_init(DFS_DEFAULT_LOCATION)` is conventionally the ROM's job in this
 * engine — examples/map-demo and examples/assets-demo both call it in
 * main(), and every downstream game has too. That convention quietly broke
 * this backend: Forge does not load
 * models by DFS path, so it had no reason to call dfs_init, and kiln_store
 * happily reported `store rom` over a filesystem that was never mounted. Every
 * read then returned "not found" whether the file was there or not, and the
 * editor started empty with a green status line.
 *
 * So this backend mounts what it needs, the same way try_sd() mounts the SD
 * volume rather than assuming someone else did. Calling dfs_init twice is
 * harmless (a ROM that already mounted it re-registers the same handle), which
 * makes this safe to add to a module every ROM links.
 */
static int try_dfs(void)
{
    int st = dfs_init(DFS_DEFAULT_LOCATION);
    /* DFS_ESUCCESS is 0. A ROM with no filesystem section at all fails here,
     * and that is worth distinguishing from an empty one: it means the backend
     * cannot serve a read even in principle. */
    return st == DFS_ESUCCESS;
}

static int try_sram(void)
{
    sram_init();
    if (sram_detect() <= 0) return 0;

    const size_t probe_off = KILN_STORE_SRAM_BYTES - SRAM_PROBE_BYTES;
    uint32_t saved = 0, pattern = 0xC0FFEE01u, back = 0;

    if (sram_read(&saved, probe_off, sizeof saved) < 0) return 0;
    if (sram_write(&pattern, probe_off, sizeof pattern) < 0) return 0;
    if (sram_read(&back, probe_off, sizeof back) < 0) return 0;
    sram_write(&saved, probe_off, sizeof saved);   /* leave it as we found it */

    if (back != pattern) return 0;

    g_sram_ok = 1;
    return 1;
}

KilnStoreKind kiln_store_init(KilnStoreKind prefer)
{
    if (g_inited) return g_kind;
    g_inited = 1;

    if (prefer >= KILN_STORE_CART_SD && try_sd()) {
        g_kind = KILN_STORE_CART_SD;
        return g_kind;
    }
    /* cart_init may have succeeded even though the card did not; keep g_cart so
     * the HUD can say "ED64, no card" rather than "no cart". */
    if (prefer >= KILN_STORE_SRAM && try_sram()) {
        g_kind = KILN_STORE_SRAM;
        return g_kind;
    }
    if (prefer >= KILN_STORE_DFS && try_dfs()) {
        g_kind = KILN_STORE_DFS;
        return g_kind;
    }
    g_kind = KILN_STORE_NONE;
    return g_kind;
}

void kiln_store_close(void)
{
    if (g_kind == KILN_STORE_CART_SD) debug_close_sdfs();
    g_kind = KILN_STORE_NONE;
    g_inited = 0;
}

KilnStoreKind kiln_store_kind(void) { return g_kind; }

int kiln_store_writable(void)
{
    return g_kind == KILN_STORE_CART_SD || g_kind == KILN_STORE_SRAM;
}

/* ── SRAM directory helpers ────────────────────────────────────────────*/

static int sram_dir_read(SramEntry *dir)
{
    if (!g_sram_ok) return KILN_STORE_ENOINIT;
    if (sram_read(dir, 0, SRAM_DIR_BYTES) < 0) return KILN_STORE_EIO;
    /* A slot claiming more than it can hold is corruption, not a big slot. */
    for (int i = 0; i < KILN_STORE_SRAM_SLOTS; i++) {
        if (dir[i].used > SRAM_SLOT_BYTES) dir[i].used = 0;
        dir[i].name[KILN_STORE_NAME_MAX - 1] = '\0';
    }
    return KILN_STORE_OK;
}

static int sram_find(const SramEntry *dir, const char *name)
{
    for (int i = 0; i < KILN_STORE_SRAM_SLOTS; i++)
        if (dir[i].used && strcmp(dir[i].name, name) == 0) return i;
    return -1;
}

static size_t sram_slot_off(int slot)
{
    return SRAM_DIR_BYTES + (size_t)slot * SRAM_SLOT_BYTES;
}

/* ── Blob write ────────────────────────────────────────────────────────*/

int kiln_store_write(const char *name, uint16_t version,
                    const void *payload, uint32_t len)
{
    if (!name_ok(name)) return KILN_STORE_ENAME;
    if (g_kind == KILN_STORE_NONE) return KILN_STORE_ENOINIT;
    if (g_kind == KILN_STORE_DFS)  return KILN_STORE_EREADONLY;

    KilnStoreHeader h = {
        .magic   = KILN_STORE_MAGIC,
        .version = version,
        .flags   = 0,
        .len     = len,
        .crc     = kiln_store_crc32(payload, len),
    };

    if (g_kind == KILN_STORE_CART_SD) {
        char path[64];
        path_for(path, sizeof path, name, "FRG");
        mkdir(KILN_STORE_DIR, 0777);
        FILE *f = fopen(path, "wb");
        if (!f) return KILN_STORE_EOPEN;
        int bad = fwrite(&h, 1, sizeof h, f) != sizeof h;
        if (!bad && len) bad = fwrite(payload, 1, len, f) != len;
        /* fclose's own return matters here: FatFs flushes the FAT and the
         * directory entry on close, so a write that "succeeded" and a close
         * that failed is a file the host will not find. */
        if (fclose(f) != 0) bad = 1;
        return bad ? KILN_STORE_EIO : KILN_STORE_OK;
    }

    /* SRAM */
    if (sizeof h + len > SRAM_SLOT_BYTES) return KILN_STORE_ENOSPACE;

    SramEntry dir[KILN_STORE_SRAM_SLOTS];
    int st = sram_dir_read(dir);
    if (st != KILN_STORE_OK) return st;

    int slot = sram_find(dir, name);
    if (slot < 0)
        for (int i = 0; i < KILN_STORE_SRAM_SLOTS && slot < 0; i++)
            if (!dir[i].used) slot = i;
    if (slot < 0) return KILN_STORE_ENOSPACE;

    size_t off = sram_slot_off(slot);
    if (sram_write(&h, off, sizeof h) < 0) return KILN_STORE_EIO;
    if (len && sram_write(payload, off + sizeof h, len) < 0) return KILN_STORE_EIO;

    /* Directory last: until it is updated the slot is still free, so an
     * interrupted write loses the new blob rather than corrupting the old
     * directory into pointing at a half-written one. */
    memset(&dir[slot], 0, sizeof dir[slot]);
    snprintf(dir[slot].name, KILN_STORE_NAME_MAX, "%s", name);
    dir[slot].used = (uint32_t)(sizeof h + len);
    if (sram_write(dir, 0, SRAM_DIR_BYTES) < 0) return KILN_STORE_EIO;
    return KILN_STORE_OK;
}

/* ── Blob read ─────────────────────────────────────────────────────────*/

static int check_header(const KilnStoreHeader *h, uint16_t version, uint32_t cap)
{
    if (h->magic != KILN_STORE_MAGIC &&
        h->magic != KILN_STORE_MAGIC_LEGACY) return KILN_STORE_EMAGIC;
    if (h->version != version)       return KILN_STORE_EVERSION;
    if (h->len > cap)                return KILN_STORE_ENOSPACE;
    return KILN_STORE_OK;
}

int kiln_store_read(const char *name, uint16_t version,
                   void *dst, uint32_t cap, uint32_t *out_len)
{
    if (!name_ok(name)) return KILN_STORE_ENAME;
    if (g_kind == KILN_STORE_NONE) return KILN_STORE_ENOINIT;

    KilnStoreHeader h;

    if (g_kind == KILN_STORE_SRAM) {
        SramEntry dir[KILN_STORE_SRAM_SLOTS];
        int st = sram_dir_read(dir);
        if (st != KILN_STORE_OK) return st;
        int slot = sram_find(dir, name);
        if (slot < 0) return KILN_STORE_ENOENT;
        size_t off = sram_slot_off(slot);
        if (sram_read(&h, off, sizeof h) < 0) return KILN_STORE_EIO;
        st = check_header(&h, version, cap);
        if (st != KILN_STORE_OK) return st;
        if (h.len && sram_read(dst, off + sizeof h, h.len) < 0)
            return KILN_STORE_EIO;
    } else {
        /* SD and DFS differ only in the path they open. */
        char path[64];
        if (g_kind == KILN_STORE_CART_SD)
            path_for(path, sizeof path, name, "FRG");
        else
            snprintf(path, sizeof path, "rom:/forge/%s.frg", name);

        FILE *f = fopen(path, "rb");
        if (!f) return KILN_STORE_ENOENT;
        int st = KILN_STORE_OK;
        if (fread(&h, 1, sizeof h, f) != sizeof h) st = KILN_STORE_EIO;
        if (st == KILN_STORE_OK) st = check_header(&h, version, cap);
        if (st == KILN_STORE_OK && h.len &&
            fread(dst, 1, h.len, f) != h.len) st = KILN_STORE_EIO;
        fclose(f);
        if (st != KILN_STORE_OK) return st;
    }

    if (kiln_store_crc32(dst, h.len) != h.crc) return KILN_STORE_ECRC;
    if (out_len) *out_len = h.len;
    return KILN_STORE_OK;
}

int kiln_store_exists(const char *name)
{
    if (!name_ok(name)) return 0;
    if (g_kind == KILN_STORE_SRAM) {
        SramEntry dir[KILN_STORE_SRAM_SLOTS];
        if (sram_dir_read(dir) != KILN_STORE_OK) return 0;
        return sram_find(dir, name) >= 0;
    }
    if (g_kind == KILN_STORE_NONE) return 0;

    char path[64];
    if (g_kind == KILN_STORE_CART_SD) path_for(path, sizeof path, name, "FRG");
    else snprintf(path, sizeof path, "rom:/forge/%s.frg", name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* ── Text ──────────────────────────────────────────────────────────────*/

int kiln_store_write_text(const char *name, const char *ext, const char *text)
{
    if (!name_ok(name) || !ext || !ext[0]) return KILN_STORE_ENAME;
    if (g_kind != KILN_STORE_CART_SD) return KILN_STORE_EREADONLY;

    char path[64];
    path_for(path, sizeof path, name, ext);
    mkdir(KILN_STORE_DIR, 0777);
    FILE *f = fopen(path, "wb");
    if (!f) return KILN_STORE_EOPEN;
    size_t n = strlen(text);
    int bad = n && fwrite(text, 1, n, f) != n;
    if (fclose(f) != 0) bad = 1;
    return bad ? KILN_STORE_EIO : KILN_STORE_OK;
}

void kiln_store_log(const char *fmt, ...)
{
    if (g_kind != KILN_STORE_CART_SD) return;
    FILE *f = fopen(KILN_STORE_DIR "/FORGE.LOG", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);   /* flushed per line: the interesting line is the last one
                  * before whatever went wrong, and a buffered log loses it. */
}

/* ── The probe ─────────────────────────────────────────────────────────*/

#define PROBE_BYTES (64 * 1024)

static void emit(KilnStoreLogFn log, void *ctx, const char *fmt, ...)
{
    char line[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (log) log(ctx, line);
}

int kiln_store_selftest(KilnStoreLogFn log, void *ctx)
{
    int fails = 0;

    emit(log, ctx, "cart  %s", kiln_store_cart_name());
    emit(log, ctx, "store %s%s", kiln_store_kind_name(),
         kiln_store_writable() ? "" : " (read only)");

    if (!kiln_store_writable()) {
        emit(log, ctx, "FAIL no writable backend");
        return 1;
    }

    uint8_t *buf = malloc(PROBE_BYTES);
    uint8_t *back = malloc(PROBE_BYTES);
    if (!buf || !back) {
        emit(log, ctx, "FAIL out of RDRAM for the probe buffers");
        free(buf); free(back);
        return 1;
    }

    /* A pattern that is wrong in a *readable* way. An all-zero or all-FF buffer
     * cannot distinguish "wrote nothing" from "wrote correctly", and a pure
     * counter cannot distinguish a sector written at the wrong LBA from one
     * written at the right one — this mixes a per-byte counter with a per-block
     * tag so the first mismatch names the block it came from. */
    for (int i = 0; i < PROBE_BYTES; i++)
        buf[i] = (uint8_t)((i * 7u) ^ (i >> 9));

    /* SRAM cannot hold 64 KB; clamp so the probe measures the backend it is
     * actually running on rather than reporting a false ENOSPACE. */
    uint32_t len = PROBE_BYTES;
    if (g_kind == KILN_STORE_SRAM && len > SRAM_SLOT_BYTES - sizeof(KilnStoreHeader))
        len = SRAM_SLOT_BYTES - sizeof(KilnStoreHeader);

    uint64_t t0 = get_ticks_ms();
    int st = kiln_store_write("PROBE", 1, buf, len);
    uint64_t t1 = get_ticks_ms();
    emit(log, ctx, "write %u B  %s  %llu ms", (unsigned)len,
         kiln_store_status_name(st), (unsigned long long)(t1 - t0));
    if (st != KILN_STORE_OK) fails++;

    if (st == KILN_STORE_OK) {
        uint32_t got = 0;
        memset(back, 0, PROBE_BYTES);
        t0 = get_ticks_ms();
        st = kiln_store_read("PROBE", 1, back, PROBE_BYTES, &got);
        t1 = get_ticks_ms();
        emit(log, ctx, "read  %u B  %s  %llu ms", (unsigned)got,
             kiln_store_status_name(st), (unsigned long long)(t1 - t0));
        if (st != KILN_STORE_OK) fails++;

        if (st == KILN_STORE_OK) {
            if (got != len) {
                emit(log, ctx, "FAIL length %u expected %u",
                     (unsigned)got, (unsigned)len);
                fails++;
            } else {
                uint32_t bad = 0;
                int first = -1;
                for (uint32_t i = 0; i < len; i++)
                    if (back[i] != buf[i]) {
                        if (first < 0) first = (int)i;
                        bad++;
                    }
                if (bad) {
                    emit(log, ctx, "FAIL %u bytes differ, first at %d",
                         (unsigned)bad, first);
                    fails++;
                } else {
                    emit(log, ctx, "verify %u B identical", (unsigned)len);
                }
            }
            /* Throughput, always — it is the number that tells you whether a
             * save will feel instant or take a visible beat.
             *
             * The bus CLASSIFICATION, though, only means something on the SD
             * backend. Its first run reported "bus sd4" off an 8 KB round trip
             * through SRAM at 1017 KB/s, which is a DMA to a chip on the
             * cartridge and has nothing to do with how libcart is clocking an
             * SD card. A plausible-looking number attached to the wrong thing
             * is worse than no number: it is the sort of reading someone later
             * quotes back as evidence. */
            uint64_t ms = (t1 - t0) ? (t1 - t0) : 1;
            uint32_t kbs = (uint32_t)((uint64_t)len / ms);  /* B/ms == KB/s */
            emit(log, ctx, "read rate %u KB/s", (unsigned)kbs);
            if (g_kind == KILN_STORE_CART_SD)
                g_bus = kbs > 200 ? "sd4" : (kbs < 60 ? "spi" : "-");
        }
    }

    /* Text emit, on the SD path only — this is the channel the .map goes out
     * through, so it is worth proving separately from the blob channel. */
    if (g_kind == KILN_STORE_CART_SD) {
        st = kiln_store_write_text("PROBE", "TXT", "kiln forge probe\n");
        emit(log, ctx, "text  %s", kiln_store_status_name(st));
        if (st != KILN_STORE_OK) fails++;
        kiln_store_log("probe: %d failure(s)", fails);
    }

    free(buf);
    free(back);

    emit(log, ctx, fails ? "RESULT FAIL" : "RESULT PASS");
    return fails;
}
