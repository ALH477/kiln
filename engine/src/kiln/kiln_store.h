/* SPDX-License-Identifier: MIT
 *
 * kiln_store.h — getting authored data OFF the console.
 *
 * Every other file-facing thing in this engine reads: kiln_engine.c opens
 * `rom:/`, kiln_map.c `dfs_open`s its .map, kiln_asset.c reads a StreamDB out of
 * ROM. That is correct for a game and useless for a tool. An on-console editor
 * that cannot write is a toy, and the whole argument for editing on hardware is
 * that hardware is where the judgements are — fill rate, palette value
 * separation, whether a corridor feels like a corridor. So this module is the
 * write half, and it is deliberately the ONLY place in the engine that knows
 * how bytes leave the machine.
 *
 * ── Three backends, one API, in preference order ───────────────────────
 *
 *   KILN_STORE_CART_SD  libcart + libdragon's FatFs, i.e. real named files on
 *                      the same SD card the ROM booted from. Read AND write,
 *                      arbitrary size. This is the good one.
 *   KILN_STORE_SRAM     the cartridge save chip, 32 KB, with a tiny fixed
 *                      directory laid over it. Survives if a flashcart clone's
 *                      SD *write* path turns out not to work; the EverDrive OS
 *                      copies the save to the card when you hold reset.
 *   KILN_STORE_DFS      `rom:/`, READ ONLY. Not a fallback for saving — it is
 *                      how the same ROM stays inspectable under an emulator,
 *                      which has no SD card at all. `./dev shot` needs this.
 *
 * `kiln_store_init(KILN_STORE_CART_SD)` walks down that list and returns what it
 * actually got. It never fails silently: the caller is expected to PRINT
 * `kiln_store_kind_name()` and `kiln_store_cart_name()` on screen, red when the
 * write path is absent. This engine has a specific history here — a ROM opened
 * `rom:/maps/pm_lab.map` while the asset shipped as `pm-lab-map.map`, every
 * layer degraded politely, and a whole screen had no collision world for its
 * entire life. A save channel that silently isn't there costs an hour of
 * building, so it is reported the way kiln_clip's brush count is.
 *
 * ── Why libcart at all, and why this cart in particular ────────────────
 *
 * The target flashcart is an ED64 Plus: no USB port, so libdragon's usb.c
 * refuses it outright (it accepts only ED64 V3 and the X-series), and with it
 * go debugf, UNFLoader and `./dev debug`. libcart is a different subsystem that
 * probes different registers, and its EverDrive driver covers "V1, V2, V2.5, V3
 * and ED64+" by name — a wide version-range gate rather than an exact magic —
 * with a real block-write driver behind it. libdragon vendors it, compiles
 * FatFs read-write, and hangs `sd:/` off newlib. So `fopen("sd:/x","wb")`
 * works on a cart that cannot speak a single byte over USB.
 *
 * ── What is deliberately NOT here ─────────────────────────────────────
 *
 * - **No 64drive / SC64 special-casing.** libcart already abstracts them and
 *   `kiln_store_cart_name()` reports which one; nothing in this module cares.
 * - **No directory enumeration on the SD path.** A tool asks for a name it
 *   already knows, or is handed one; walking the card's FAT to build a browser
 *   is a UI feature, not a storage one, and it is the part most likely to hold
 *   a dirent open across a write.
 * - **No FlashRAM.** libdragon has no FlashRAM API at all — the string is
 *   accepted by nix/rom.nix's `saveType` and understood only by
 *   ed64romconfig's header stamp. Do not add `flashram` expecting it to work.
 * - **No async or partial writes.** A save is one call that either wrote every
 *   byte or reports which step failed. There is no half-saved level.
 * - **No compression.** The .FRG payloads are small and RLE'd by their own
 *   serialiser; a second layer here would only obscure the CRC.
 */
#ifndef KILN_STORE_H
#define KILN_STORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sizes ─────────────────────────────────────────────────────────────
 *
 * SRAM is 32 KB and libdragon's sram.c hardcodes that regardless of the
 * declared save type, so the SRAM backend's directory and payload budget are
 * fixed at build time rather than probed. Four entries is the same shape (and
 * the same reasoning) as kiln_save.h's four EEPROM slots: a fixed table with no
 * allocator, sized so the arithmetic is checkable by eye.
 */
#define KILN_STORE_SRAM_BYTES   0x8000  /* 32 KB, libdragon's fixed SRAM size  */
#define KILN_STORE_SRAM_SLOTS   4       /* directory entries in the SRAM image */
#define KILN_STORE_NAME_MAX     24      /* including the terminator            */

/* A blob's on-medium header. Little of this is optional: `len` is what makes a
 * short read detectable, `crc` is what makes a bad SD sector detectable, and
 * `version` is what stops a newer editor from confidently misreading an older
 * level. All three have to be checked on read or none of them are worth
 * writing. Stored big-endian-native (this is a MIPS target and the host tools
 * that parse it are told so explicitly in tools/forge/frg.py).
 */
/* 'KLNS'. The pre-rename magic was 'M64S' and is still ACCEPTED on read: a
 * project rename must not strand a level already written to a flashcart. New
 * writes always use the current magic, so a card round-trips forward but not
 * back — the direction that matters, since the ROM is what gets rebuilt. */
#define KILN_STORE_MAGIC        0x4B4C4E53u  /* 'KLNS'                        */
#define KILN_STORE_MAGIC_LEGACY 0x4D363453u  /* 'M64S', pre-rename, read-only */

typedef struct {
    uint32_t magic;    /* KILN_STORE_MAGIC                                  */
    uint16_t version;  /* payload format version, the caller's own number  */
    uint16_t flags;    /* reserved, written 0                              */
    uint32_t len;      /* payload bytes following this header              */
    uint32_t crc;      /* CRC-32 (IEEE, reflected) over exactly `len`      */
} KilnStoreHeader;

typedef enum {
    KILN_STORE_NONE = 0,    /* nothing worked; every call will fail          */
    KILN_STORE_DFS,         /* rom:/, read-only                             */
    KILN_STORE_SRAM,        /* cartridge save chip, 32 KB, read/write       */
    KILN_STORE_CART_SD,     /* flashcart SD card, read/write, named files    */
} KilnStoreKind;

/* Status codes. Negative, distinct, and each one names a step rather than a
 * feeling — "it didn't save" is the report that wastes the afternoon.
 */
typedef enum {
    KILN_STORE_OK          =  0,
    KILN_STORE_ENOINIT     = -1,  /* kiln_store_init not called / found nothing */
    KILN_STORE_EREADONLY   = -2,  /* this backend cannot write (DFS)           */
    KILN_STORE_EOPEN       = -3,  /* could not open/create the target          */
    KILN_STORE_EIO         = -4,  /* a read or write returned short            */
    KILN_STORE_ENOSPACE    = -5,  /* payload does not fit this backend         */
    KILN_STORE_ENOENT      = -6,  /* no such blob                              */
    KILN_STORE_EMAGIC      = -7,  /* not a Kiln store blob                     */
    KILN_STORE_EVERSION    = -8,  /* blob version != requested version         */
    KILN_STORE_ECRC        = -9,  /* payload failed its own checksum           */
    KILN_STORE_ENAME       = -10, /* name too long or empty                    */
} KilnStoreStatus;

/* ── Lifecycle ─────────────────────────────────────────────────────────*/

/* Probe, in preference order, down to and including `prefer`'s weaker
 * neighbours. Returns the kind actually established (possibly KILN_STORE_NONE).
 * Safe to call more than once; the second call is a no-op returning the same
 * kind. Calls cart_init() itself so kiln_store_cart_name() has something to
 * report even when the SD mount is the thing that failed.
 */
KilnStoreKind kiln_store_init(KilnStoreKind prefer);

/* Unmount cleanly. Call before the user is told it is safe to power off — a
 * FatFs volume with buffered metadata and a yanked card is how a level and the
 * ROM next to it are lost together.
 */
void kiln_store_close(void);

KilnStoreKind kiln_store_kind(void);
int          kiln_store_writable(void);

/* Short, screen-safe strings. Both are alphanumeric plus '.' '-' '/' and '+'
 * only: FONT_BUILTIN_DEBUG_MONO has no '@' glyph and renders it as '0', so a
 * status line that reads "2@5" comes out as "205". Never NULL.
 */
const char *kiln_store_kind_name(void);   /* "sd" / "sram" / "rom" / "none"   */
const char *kiln_store_cart_name(void);   /* "ED64+" / "SC64" / "none"        */
const char *kiln_store_status_name(int status);

/* Which SD bus mode the cart appears to be using — libcart falls back from
 * 4-bit SD to SPI when the cart's bootloader label is not the stock one, which
 * is slow but works, and on a clone it is the likely case. "sd4" / "spi" / "-".
 *
 * INFERRED from throughput measured by kiln_store_selftest, not read from a
 * register: libcart keeps that decision in a file-static and the register
 * behind it lives inside libcart's own PI timing save/restore, so reading it
 * from outside means poking DOM2 while another subsystem believes it owns the
 * timing. A wrong guess there hangs the console, and on a cart with no USB a
 * hang is indistinguishable from a bad ROM. "-" until the probe has run.
 */
const char *kiln_store_bus_name(void);

/* ── Blobs (headered, checksummed) ─────────────────────────────────────
 *
 * `name` is a bare name with no directory and no extension — the backend owns
 * the layout, because the SRAM backend has no directories to own. The SD
 * backend writes KILN_STORE_DIR/<name>.<ext>. Passing a path separator is
 * KILN_STORE_ENAME rather than a silently different file: an asset's name IS
 * its filename is a lesson this repo paid for once already.
 */
#define KILN_STORE_DIR "sd:/FORGE"

int kiln_store_write(const char *name, uint16_t version,
                    const void *payload, uint32_t len);

/* Reads into `dst`, verifying magic, version and CRC before reporting success.
 * `*out_len` is set on success only. A payload longer than `cap` is
 * KILN_STORE_ENOSPACE and reads nothing — a truncated level that loads is worse
 * than one that refuses.
 */
int kiln_store_read(const char *name, uint16_t version,
                   void *dst, uint32_t cap, uint32_t *out_len);

int kiln_store_exists(const char *name);

/* ── Text ──────────────────────────────────────────────────────────────
 *
 * The derived artifacts — Quake .map, a PMCamKey C fragment, an entity list —
 * are emitted as plain ASCII in the dialect the repo already pins, so the host
 * side is a copy plus `./dev map-validate` and not a bespoke decoder. Those
 * carry no KilnStoreHeader for exactly that reason: a header would make them
 * un-diffable and un-`cat`-able, which is most of their value.
 *
 * SD only. On SRAM this returns KILN_STORE_EREADONLY: 32 KB is the working
 * format's budget, not the working format plus its expansions.
 */
int kiln_store_write_text(const char *name, const char *ext, const char *text);

/* Append one line to KILN_STORE_DIR/FORGE.LOG, flushed immediately. This is the
 * replacement for debugf() on a cart with no USB — the only other place a
 * diagnostic can go is the screen, and the screen cannot hold a session's
 * history. No-op unless the backend is SD.
 */
void kiln_store_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ── The probe ─────────────────────────────────────────────────────────
 *
 * Writes a pattern, reads it back, compares, and reports every step. This is
 * the FIRST thing to run on an unfamiliar flashcart, because whether a clone's
 * SD write path works is the one assumption everything above rests on, and it
 * is answerable in one boot. It exercises this module rather than a parallel
 * implementation, so a PASS is evidence about the code the editor will use.
 *
 * `log` is called once per step with a short line; `Forge/selftest` points it
 * at libdragon's console. Returns 0 if every step passed.
 */
typedef void (*KilnStoreLogFn)(void *ctx, const char *line);

int kiln_store_selftest(KilnStoreLogFn log, void *ctx);

/* CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor). Exposed because
 * the .FRG serialiser wants to checksum sub-sections, and because a second
 * implementation of a checksum is a way to have two answers.
 */
uint32_t kiln_store_crc32(const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* KILN_STORE_H */
