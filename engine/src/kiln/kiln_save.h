/* SPDX-License-Identifier: MIT
 *
 * kiln_save.h — save slots on EEPROM.
 *
 * ── Why this is in the engine and not in a game ────────────────────────
 * Two downstream games wanted the same thing and would otherwise have
 * written it twice: an OoT-style profile save (three files, and whether a
 * file exists is what decides if the intro plays) for one, and match
 * progress plus per-character unlock flags for the other. Neither needed
 * anything the other did not. What IS game-specific — how big a slot is,
 * what goes in it, what the screen looks like — stays a parameter.
 *
 * ── A thin layer over eepromfs, not a replacement ──────────────────────
 * libdragon already ships eepromfs (`eepromfs.h`), which handles block
 * addressing, the 4k/16k difference, a per-file checksum, and — with the
 * `backup` flag — a second copy that is used automatically when the first
 * is corrupt. Power-loss safety is therefore NOT this module's job and is
 * deliberately not reimplemented here; both flags are set on every slot.
 *
 * What this adds is the three things eepfs cannot know about:
 *
 *   1. A slot table built from two numbers, so a game declares "3 slots of
 *      N bytes" instead of hand-writing eepfs_entry_t paths and keeping
 *      their sizes in sync with the struct it actually stores.
 *   2. Magic + version ahead of the payload. eepfs's checksum catches
 *      corruption but cannot catch a slot written by an OLDER BUILD of
 *      the game, which passes its checksum and then deserialises into a
 *      struct that has since changed shape. That is the version's job.
 *   3. kiln_save_exists(), which is the actual question a file-select
 *      screen asks, and which no combination of eepfs calls answers
 *      directly.
 *
 * ── The header is fixed and versioned ──────────────────────────────────
 * Every slot carries KilnSaveHeader ahead of the game's payload. `version`
 * is the game's own number: bump it when the payload layout changes and
 * old slots read as empty instead of being reinterpreted, which is the
 * cheap version of a migration and the right one for a console game.
 *
 * ── Budget ─────────────────────────────────────────────────────────────
 * EEPROM 4k is 512 bytes TOTAL, in 64 blocks of 8, of which eepfs reserves
 * the first for its signature — 504 usable. Each slot costs
 * 4 (header) + payload, rounded up to a block, and then DOUBLED because
 * `backup` is on. Three slots of 32 payload bytes is 3 * 2 * 40 = 240
 * bytes and fits; three slots of 96 does not.
 *
 * kiln_save_init returns -1 rather than truncating when the table does not
 * fit, because a save system that silently drops the end of every write is
 * worse than one that refuses to start.
 */
#ifndef KILN_SAVE_H
#define KILN_SAVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Hard ceiling on slots. Three is OoT's number and all any game here has
 *  wanted; the cap exists so the entry table can be static. */
#define KILN_SAVE_MAX_SLOTS 4

/** Written ahead of every slot's payload. Games do not fill this in —
 *  kiln_save_write does. */
typedef struct {
    uint16_t magic;   /**< KILN_SAVE_MAGIC when the slot was written        */
    uint16_t version; /**< the game's payload version                       */
} KilnSaveHeader;

#define KILN_SAVE_MAGIC 0x4B4Cu /* 'KL' */

/** Bring up eepromfs with `slots` files of `payload_size` bytes each.
 *  `version` is the game's payload version (see the header comment).
 *
 *  Returns 0 on success, -1 if the console has no EEPROM, if the table
 *  does not fit, or if eepromfs refuses the layout. A game that cannot
 *  save is still playable, so callers should degrade rather than abort —
 *  check the return and disable the file-select screen's write paths. */
int kiln_save_init(uint16_t slots, uint16_t payload_size, uint16_t version);

/** Number of slots this game declared, 0 before a successful init. */
uint16_t kiln_save_slots(void);

/** 1 if `slot` holds a valid save for the current version, else 0.
 *  Out-of-range slots return 0. This is the file-select screen's question:
 *  an empty slot is what starts a new game. */
int kiln_save_exists(int slot);

/** Read a slot's payload into `dest` (payload_size bytes).
 *  Returns 0 on success, -1 if the slot is empty, corrupt, the wrong
 *  version, or out of range — in every one of those cases `dest` is left
 *  untouched, so a failed load cannot half-populate a game state. */
int kiln_save_read(int slot, void *dest);

/** Write `src` (payload_size bytes) to a slot, stamping header + checksum.
 *  Returns 0 on success, -1 on a bad slot or a write error. */
int kiln_save_write(int slot, const void *src);

/** Erase a slot so kiln_save_exists reports 0. Returns 0 on success. */
int kiln_save_erase(int slot);

#ifdef __cplusplus
}
#endif

#endif /* KILN_SAVE_H */
