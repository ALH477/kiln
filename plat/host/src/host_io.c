/* SPDX-License-Identifier: MIT
 *
 * host_io.c — DragonFS, the joypad, EEPROM, sprites and the flashcart, on the
 * host.
 *
 * Five small surfaces in one file because they share a theme: each is a place
 * the console reaches outside the CPU, and each has an obvious host answer
 * that is NOT an approximation of the console's behaviour but the whole of what
 * the abstraction means off-console.
 *
 *   DragonFS  a directory. On console it is a filesystem image appended to the
 *             ROM; the mapping onto real files is the useful part, because
 *             CLAUDE.md records that an asset builder's `name` IS the filename
 *             a ROM must open and that a mismatch cost PetaByte Madness its
 *             entire PLAY screen. Here that mismatch is a missing file you can
 *             see with ls.
 *   joypad     no physical controller exists in a Nix sandbox, so the state is
 *             settable from a test. That is what lets kiln_input's edge
 *             detection be exercised at all.
 *   EEPROM     one file, with the console's 4/16 Kbit sizes ENFORCED. A host
 *             that let a save grow would answer the wrong question — fitting
 *             into 512 bytes is the entire problem kiln_save exists for.
 *   sprites    real .sprite parsing, because kiln_texanim reaches into width,
 *             height and the format bits.
 *   libcart    reports no cart, loudly rather than by pretending. See
 *             plat/host/include/libcart/cart.h.
 */
#include <libdragon.h>
#include <libcart/cart.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── DragonFS ─────────────────────────────────────────────────────────── */

#define DFS_MAX_OPEN 32

static struct { FILE *fp; long size; int used; } g_dfs[DFS_MAX_OPEN];
static int  g_dfs_ready;
static char g_dfs_root[512] = ".";

int dfs_init(pi_addr_t base_fs_loc)
{
    (void)base_fs_loc;
    const char *r = getenv("KILN_HOST_DFS");
    if (r && *r) snprintf(g_dfs_root, sizeof g_dfs_root, "%s", r);
    g_dfs_ready = 1;
    return DFS_ESUCCESS;
}

/* "rom:/models/x.t3dm" and "models/x.t3dm" both resolve under the root. The
 * prefix is stripped rather than rejected because every engine call site
 * spells it, and a host build that refused them would need #ifdefs in engine
 * code — which is the one thing plat/host exists to avoid. */
static void resolve(char *out, size_t n, const char *path)
{
    const char *p = path;
    if (strncmp(p, "rom:/", 5) == 0) p += 5;
    else if (strncmp(p, "rom:", 4) == 0) p += 4;
    while (*p == '/') p++;
    snprintf(out, n, "%s/%s", g_dfs_root, p);
}

int dfs_open(const char *const path)
{
    if (!path) return DFS_EBADINPUT;
    if (!g_dfs_ready) return DFS_ENOINIT;

    char full[1024];
    resolve(full, sizeof full, path);
    FILE *fp = fopen(full, "rb");
    if (!fp) {
        /* debugf and not silence: kiln_map_load returns non-zero rather than
         * asserting, so a missing asset is a POLITE failure all the way down.
         * Saying which path failed is the difference between "PLAY has no
         * collision world" and an afternoon. */
        debugf("dfs_open: '%s' -> '%s': %s\n", path, full, strerror(errno));
        return DFS_ENOFILE;
    }
    for (int i = 0; i < DFS_MAX_OPEN; i++) {
        if (g_dfs[i].used) continue;
        fseek(fp, 0, SEEK_END);
        g_dfs[i].size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        g_dfs[i].fp = fp;
        g_dfs[i].used = 1;
        return i + 1;    /* handle 0 is reserved: see kiln_cache's lesson */
    }
    fclose(fp);
    assertf(0, "dfs_open: more than %d files open at once", DFS_MAX_OPEN);
    return DFS_EBADINPUT;
}

static int handle_ok(uint32_t h) { return h >= 1 && h <= DFS_MAX_OPEN && g_dfs[h-1].used; }

int dfs_read(void *const buf, int size, int count, uint32_t handle)
{
    if (!handle_ok(handle) || !buf) return DFS_EBADINPUT;
    return (int)fread(buf, (size_t)size, (size_t)count, g_dfs[handle-1].fp) * size;
}

int dfs_close(uint32_t handle)
{
    if (!handle_ok(handle)) return DFS_EBADINPUT;
    fclose(g_dfs[handle-1].fp);
    g_dfs[handle-1].used = 0;
    return DFS_ESUCCESS;
}

int dfs_size(uint32_t handle)
{
    if (!handle_ok(handle)) return DFS_EBADINPUT;
    return (int)g_dfs[handle-1].size;
}

int dfs_seek(uint32_t handle, int offset, int origin)
{
    if (!handle_ok(handle)) return DFS_EBADINPUT;
    return fseek(g_dfs[handle-1].fp, offset, origin) == 0 ? DFS_ESUCCESS
                                                          : DFS_EBADINPUT;
}
int dfs_tell(uint32_t handle)
{
    if (!handle_ok(handle)) return DFS_EBADINPUT;
    return (int)ftell(g_dfs[handle-1].fp);
}
int dfs_eof(uint32_t handle)
{
    if (!handle_ok(handle)) return DFS_EBADINPUT;
    return feof(g_dfs[handle-1].fp) ? 1 : 0;
}

/* ── joypad ───────────────────────────────────────────────────────────── */

static joypad_inputs_t g_pad[JOYPAD_PORT_COUNT];
static joypad_inputs_t g_pad_live[JOYPAD_PORT_COUNT];
static joypad_buttons_t g_pad_prev[JOYPAD_PORT_COUNT];
static int g_pad_connected[JOYPAD_PORT_COUNT];

void joypad_init(void)
{
    memset(g_pad, 0, sizeof g_pad);
    memset(g_pad_live, 0, sizeof g_pad_live);
    memset(g_pad_prev, 0, sizeof g_pad_prev);
    /* Port 1 only. Four connected pads would make kiln_input's per-port loop
     * exercise ports that a host test never sets, and "why is player 3
     * walking" is a bad afternoon. */
    g_pad_connected[0] = 1;
}

void joypad_close(void) { }

void joypad_poll(void)
{
    /* One latch per frame, matching libdragon: joypad_get_* must return the
     * same values for the whole frame, which is exactly what kiln_input's
     * one-poll-per-frame contract depends on. */
    for (int i = 0; i < JOYPAD_PORT_COUNT; i++) {
        g_pad_prev[i] = g_pad[i].btn;
        g_pad[i] = g_pad_live[i];
    }
}

void kiln_host_pad_set(joypad_port_t port, joypad_inputs_t in)
{
    assertf((int)port >= 0 && (int)port < JOYPAD_PORT_COUNT,
            "kiln_host_pad_set: port %d", (int)port);
    g_pad_live[port] = in;
    g_pad_connected[port] = 1;
}

joypad_inputs_t joypad_get_inputs(joypad_port_t port)
{
    if ((int)port < 0 || (int)port >= JOYPAD_PORT_COUNT)
        return (joypad_inputs_t){ 0 };
    return g_pad[port];
}
joypad_buttons_t joypad_get_buttons(joypad_port_t port)
{
    return joypad_get_inputs(port).btn;
}
joypad_buttons_t joypad_get_buttons_pressed(joypad_port_t port)
{
    joypad_buttons_t r = { 0 };
    if ((int)port < 0 || (int)port >= JOYPAD_PORT_COUNT) return r;
    r.raw = (uint16_t)(g_pad[port].btn.raw & ~g_pad_prev[port].raw);
    return r;
}
joypad_buttons_t joypad_get_buttons_released(joypad_port_t port)
{
    joypad_buttons_t r = { 0 };
    if ((int)port < 0 || (int)port >= JOYPAD_PORT_COUNT) return r;
    r.raw = (uint16_t)(~g_pad[port].btn.raw & g_pad_prev[port].raw);
    return r;
}
joypad_buttons_t joypad_get_buttons_held(joypad_port_t port)
{
    joypad_buttons_t r = { 0 };
    if ((int)port < 0 || (int)port >= JOYPAD_PORT_COUNT) return r;
    r.raw = (uint16_t)(g_pad[port].btn.raw & g_pad_prev[port].raw);
    return r;
}
bool joypad_is_connected(joypad_port_t port)
{
    return (int)port >= 0 && (int)port < JOYPAD_PORT_COUNT && g_pad_connected[port];
}

/* ── EEPROM filesystem ────────────────────────────────────────────────── */

#define EEP_BLOCK 8
#define EEP_16K_BYTES 2048

static const eepfs_entry_t *g_eep_entries;
static size_t g_eep_count;
static uint8_t g_eep[EEP_16K_BYTES];
static int g_eep_ready;
static char g_eep_path[512] = "kiln-eeprom.bin";

eeprom_type_t eeprom_present(void) { return EEPROM_16K; }
size_t eeprom_total_blocks(void)   { return EEP_16K_BYTES / EEP_BLOCK; }

static void eep_flush(void)
{
    FILE *f = fopen(g_eep_path, "wb");
    if (!f) { debugf("eepfs: cannot write '%s': %s\n", g_eep_path, strerror(errno)); return; }
    if (fwrite(g_eep, 1, sizeof g_eep, f) != sizeof g_eep)
        debugf("eepfs: short write to '%s'\n", g_eep_path);
    fclose(f);
}

int eepfs_init(const eepfs_entry_t *entries, size_t count)
{
    if (!entries || count == 0) return EEPFS_EBADINPUT;

    size_t total = 1;   /* upstream reserves the first block for a signature */
    for (size_t i = 0; i < count; i++) {
        if (!entries[i].path || entries[i].size == 0) return EEPFS_EBADINPUT;
        size_t blocks = (entries[i].size + EEP_BLOCK - 1) / EEP_BLOCK;
        if (entries[i].backup) blocks *= 2;
        total += blocks;
    }
    /* The console limit, enforced. Fitting into it is the entire problem
     * kiln_save exists to solve, so a host that quietly allowed more would be
     * answering a question nobody asked. */
    assertf(total * EEP_BLOCK <= EEP_16K_BYTES,
            "eepfs_init: %zu entries need %zu blocks (%zu bytes), and a 16 Kbit "
            "EEPROM has %zu (%d bytes)", count, total, total * EEP_BLOCK,
            eeprom_total_blocks(), EEP_16K_BYTES);

    const char *p = getenv("KILN_HOST_EEPROM");
    if (p && *p) snprintf(g_eep_path, sizeof g_eep_path, "%s", p);

    memset(g_eep, 0, sizeof g_eep);
    FILE *f = fopen(g_eep_path, "rb");
    if (f) {
        /* A short read is a truncated save, not nothing: the tail stays zero,
         * which is what eepfs_verify_signature is there to notice. */
        const size_t got = fread(g_eep, 1, sizeof g_eep, f);
        if (got != sizeof g_eep)
            debugf("eepfs_init: '%s' held %zu of %zu bytes\n", g_eep_path, got,
                   sizeof g_eep);
        fclose(f);
    }

    g_eep_entries = entries;
    g_eep_count = count;
    g_eep_ready = 1;
    return EEPFS_ESUCCESS;
}

void eepfs_close(void) { g_eep_ready = 0; g_eep_entries = NULL; g_eep_count = 0; }

static int eep_find(const char *path, size_t *off, size_t *size)
{
    if (!g_eep_ready || !path) return -1;
    const char *p = path;
    while (*p == '/') p++;
    size_t at = EEP_BLOCK;   /* after the signature block */
    for (size_t i = 0; i < g_eep_count; i++) {
        const char *q = g_eep_entries[i].path;
        while (*q == '/') q++;
        size_t blocks = (g_eep_entries[i].size + EEP_BLOCK - 1) / EEP_BLOCK;
        if (g_eep_entries[i].backup) blocks *= 2;
        if (strcmp(p, q) == 0) { *off = at; *size = g_eep_entries[i].size; return 0; }
        at += blocks * EEP_BLOCK;
    }
    return -1;
}

int eepfs_read(const char *path, void *dest, size_t size)
{
    size_t off, esz;
    if (!dest) return EEPFS_EBADINPUT;
    if (eep_find(path, &off, &esz) != 0) return EEPFS_ENOFILE;
    if (size > esz) return EEPFS_EBADINPUT;
    memcpy(dest, &g_eep[off], size);
    return EEPFS_ESUCCESS;
}

int eepfs_write(const char *path, const void *src, size_t size)
{
    size_t off, esz;
    if (!src) return EEPFS_EBADINPUT;
    if (eep_find(path, &off, &esz) != 0) return EEPFS_ENOFILE;
    if (size > esz) return EEPFS_EBADINPUT;
    memcpy(&g_eep[off], src, size);
    eep_flush();
    return EEPFS_ESUCCESS;
}

int eepfs_erase(const char *path)
{
    size_t off, esz;
    if (eep_find(path, &off, &esz) != 0) return EEPFS_ENOFILE;
    memset(&g_eep[off], 0, esz);
    eep_flush();
    return EEPFS_ESUCCESS;
}

bool eepfs_verify_signature(void)
{
    /* Upstream hashes the entry table into block 0 so a layout change is
     * detected rather than silently reinterpreting old bytes. Same idea, and
     * the same consequence when it fails: kiln_save wipes and starts over. */
    if (!g_eep_ready) return false;
    uint32_t sig = 0x4B4C4E53u;  /* 'KLNS' */
    for (size_t i = 0; i < g_eep_count; i++) {
        for (const char *c = g_eep_entries[i].path; *c; c++)
            sig = sig * 31u + (uint32_t)(unsigned char)*c;
        sig = sig * 31u + (uint32_t)g_eep_entries[i].size;
    }
    uint32_t stored;
    memcpy(&stored, g_eep, sizeof stored);
    if (stored == sig) return true;
    memcpy(g_eep, &sig, sizeof sig);
    eep_flush();
    return false;
}

void eepfs_wipe(void)
{
    memset(g_eep, 0, sizeof g_eep);
    eep_flush();
}

/* ── sprites ──────────────────────────────────────────────────────────── */

sprite_t *sprite_load_buf(void *buf, int sz)
{
    assertf(buf != NULL && sz >= 8, "sprite_load_buf: %d bytes is too small", sz);
    /* .sprite is big-endian, same as everything else built for MIPS. Convert
     * the header in place — the pixel payload is bytes and needs none. */
    uint8_t *b = buf;
    sprite_t *s = (sprite_t *)buf;
    const uint16_t w = (uint16_t)((b[0] << 8) | b[1]);
    const uint16_t h = (uint16_t)((b[2] << 8) | b[3]);
    s->width = w; s->height = h;
    return s;
}

sprite_t *sprite_load(const char *path)
{
    const int h = dfs_open(path);
    assertf(h > 0, "sprite_load: cannot open '%s'", path);
    const int sz = dfs_size((uint32_t)h);
    void *buf = malloc((size_t)sz);
    assertf(buf != NULL, "sprite_load: out of memory for '%s'", path);
    const int got = dfs_read(buf, 1, sz, (uint32_t)h);
    dfs_close((uint32_t)h);
    assertf(got == sz, "sprite_load: '%s' read %d of %d bytes", path, got, sz);
    return sprite_load_buf(buf, sz);
}

void sprite_free(sprite_t *s) { free(s); }

surface_t sprite_get_pixels(sprite_t *s)
{
    assertf(s != NULL, "sprite_get_pixels: NULL");
    return surface_make_linear((void *)s->data, sprite_get_format(s),
                               s->width, s->height);
}

uint16_t *sprite_get_palette(sprite_t *s)
{
    (void)s;
    /* The palette lives in the sprite's extended header, which this reader does
     * not parse yet. Returning NULL would be read as "no palette" and silently
     * draw the raw indices as intensity. */
    assertf(0, "sprite_get_palette is not implemented on the host: the .sprite "
               "extended header is not parsed yet.");
    return NULL;
}

/* ── libcart ──────────────────────────────────────────────────────────── */

int cart_type = CART_NULL;

int cart_init(void)      { cart_type = CART_NULL; return -1; }
int cart_exit(void)      { return -1; }
int cart_card_init(void) { return -1; }
int cart_card_rd_dram(void *d, uint32_t lba, uint32_t n)
{ (void)d; (void)lba; (void)n; return -1; }
int cart_card_wr_dram(const void *d, uint32_t lba, uint32_t n)
{ (void)d; (void)lba; (void)n; return -1; }

/* ── SRAM ─────────────────────────────────────────────────────────────
 * No save chip, reported the way libdragon actually reports it. */
void sram_init(void) { }
int  sram_detect(void) { return 0; }
int  sram_read(void *dst, size_t off, size_t len)
{ (void)dst; (void)off; (void)len; return -1; }
int  sram_write(const void *src, size_t off, size_t len)
{ (void)src; (void)off; (void)len; return -1; }

/* ── libdragon's debug SD surface ─────────────────────────────────────
 * No flashcart, so no mount. Returning false is what kiln_store's backend
 * walk is written for. */
bool debug_init_sdfs(const char *prefix, int npart)
{ (void)prefix; (void)npart; return false; }
void debug_close_sdfs(void) { }
bool debug_init_usblog(void)   { return false; }
bool debug_init_isviewer(void) { return false; }
