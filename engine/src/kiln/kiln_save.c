/* SPDX-License-Identifier: MIT
 *
 * kiln_save.c — see kiln_save.h.
 */

#include "kiln_save.h"

#include <libdragon.h>
#include <string.h>

/* eepfs takes a table of paths that must outlive eepfs_init, so the paths
 * are string literals rather than something built at runtime. Four is
 * KILN_SAVE_MAX_SLOTS; a game asking for fewer uses a prefix of this. */
static const char *const SLOT_PATH[KILN_SAVE_MAX_SLOTS] = {
    "/slot0", "/slot1", "/slot2", "/slot3",
};

static eepfs_entry_t g_entries[KILN_SAVE_MAX_SLOTS];
static uint16_t g_slots;
static uint16_t g_payload;
static uint16_t g_version;

/* One slot's bytes: header then payload. Sized at init and used as the
 * staging buffer for both directions, because eepfs reads and writes whole
 * files and the header has to travel with the payload either way.
 *
 * Static rather than alloca'd per call: this is a console, the size is
 * known at init, and a save path that can fail on a fragmented heap is a
 * save path that fails exactly when the player most wants it to work. */
#define SLOT_BYTES_MAX 128
static uint8_t g_slot_buf[SLOT_BYTES_MAX];

static inline int slot_bytes(void)
{
    return (int)sizeof(KilnSaveHeader) + (int)g_payload;
}

static inline int slot_ok(int slot)
{
    return g_slots != 0 && slot >= 0 && slot < (int)g_slots;
}

int kiln_save_init(uint16_t slots, uint16_t payload_size, uint16_t version)
{
    g_slots = 0;

    if (slots == 0 || slots > KILN_SAVE_MAX_SLOTS) return -1;
    if ((int)sizeof(KilnSaveHeader) + (int)payload_size > SLOT_BYTES_MAX) {
        debugf("kiln_save: %d-byte slot exceeds the %d-byte staging buffer\n",
               (int)sizeof(KilnSaveHeader) + payload_size, SLOT_BYTES_MAX);
        return -1;
    }

    /* No cartridge EEPROM means no saves. Not an error to shout about —
     * this is also what an emulator with the save type unset looks like —
     * but the caller has to know so it can hide the file-select screen's
     * write paths rather than offering a save that silently vanishes. */
    if (eeprom_present() == EEPROM_NONE) {
        debugf("kiln_save: no EEPROM present\n");
        return -1;
    }

    const int bytes = (int)sizeof(KilnSaveHeader) + (int)payload_size;
    for (uint16_t i = 0; i < slots; i++) {
        g_entries[i].path = SLOT_PATH[i];
        g_entries[i].size = (size_t)bytes;
        /* Both on, always. eepfs's own checksum is what makes a
         * never-written slot read as corrupt (and therefore empty) rather
         * than as plausible garbage, and the backup copy is what survives
         * losing power mid-write. Neither is worth a per-game switch. */
        g_entries[i].checksum = true;
        g_entries[i].backup = true;
    }

    if (eepfs_init(g_entries, slots) != EEPFS_ESUCCESS) {
        /* Almost always the table not fitting: 4k EEPROM leaves 504 bytes,
         * and `backup` doubles every slot. See kiln_save.h's budget note. */
        debugf("kiln_save: eepfs_init refused %u slots of %d bytes "
               "(backup doubles it; 4k EEPROM has 504 usable)\n",
               slots, bytes);
        return -1;
    }

    g_slots = slots;
    g_payload = payload_size;
    g_version = version;

    /* A cartridge whose EEPROM was last used by a DIFFERENT game has data
     * that is not ours and will not match this layout. eepfs detects that
     * via its signature; wiping is the only correct response, and doing it
     * here means kiln_save_exists never reports another game's bytes as a
     * playable profile. */
    if (!eepfs_verify_signature()) {
        debugf("kiln_save: EEPROM signature mismatch, wiping\n");
        eepfs_wipe();
    }
    return 0;
}

uint16_t kiln_save_slots(void) { return g_slots; }

/* Read a slot into the staging buffer and validate its header.
 * Returns 0 when the slot holds a valid save for this version. */
static int slot_load(int slot)
{
    if (!slot_ok(slot)) return -1;
    if (eepfs_read(SLOT_PATH[slot], g_slot_buf, (size_t)slot_bytes())
        != EEPFS_ESUCCESS)
        return -1; /* never written, or corrupt past the backup copy */

    KilnSaveHeader h;
    memcpy(&h, g_slot_buf, sizeof h);
    if (h.magic != KILN_SAVE_MAGIC) return -1;
    if (h.version != g_version) {
        /* Written by an older build whose payload had a different shape.
         * Reporting it empty is the migration: the player loses the file,
         * which is bad, but loading it into a struct that has since gained
         * a field is worse and much harder to diagnose. */
        debugf("kiln_save: slot %d is version %u, this build wants %u\n",
               slot, h.version, g_version);
        return -1;
    }
    return 0;
}

int kiln_save_exists(int slot)
{
    return slot_load(slot) == 0;
}

int kiln_save_read(int slot, void *dest)
{
    if (slot_load(slot) != 0) return -1;
    /* Only now, after the header validated — a failed load must not
     * half-populate the caller's state. */
    memcpy(dest, g_slot_buf + sizeof(KilnSaveHeader), g_payload);
    return 0;
}

int kiln_save_write(int slot, const void *src)
{
    if (!slot_ok(slot)) return -1;

    KilnSaveHeader h = { .magic = KILN_SAVE_MAGIC, .version = g_version };
    memcpy(g_slot_buf, &h, sizeof h);
    memcpy(g_slot_buf + sizeof h, src, g_payload);

    if (eepfs_write(SLOT_PATH[slot], g_slot_buf, (size_t)slot_bytes())
        != EEPFS_ESUCCESS) {
        debugf("kiln_save: write to slot %d failed\n", slot);
        return -1;
    }
    return 0;
}

int kiln_save_erase(int slot)
{
    if (!slot_ok(slot)) return -1;
    return eepfs_erase(SLOT_PATH[slot]) == EEPFS_ESUCCESS ? 0 : -1;
}
