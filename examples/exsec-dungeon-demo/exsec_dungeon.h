/* SPDX-License-Identifier: MIT
 *
 * The C face of examples/dungeon/dungeon.exsc, emitted by the Exsecutor
 * compiler's C backend as dungeon_mips64.gen.c (the console) and
 * dungeon_x86_64.gen.c (the host check). See PROVENANCE.md.
 *
 * EVERY INTEGER CROSSES AS uint64_t, on both rows. The C backend holds every
 * `uN` and `iN` in a uint64_t carrier, canonical form, and that is the ABI:
 * `mensura` is 32 bits on the console row and the value is still passed and
 * returned in a uint64_t. So one header serves both units, and a prototype that
 * disagrees with either is a compile error in exsec_proto_check.c.
 *
 * `stats` is an `acies<u64, 3>`: pass a uint64_t[3]. The unit reads and writes
 * it through memcpy, so alignment is not required, but a uint64_t array has it.
 */
#ifndef EXSEC_DUNGEON_H
#define EXSEC_DUNGEON_H

#include <stdint.h>

#define EXSEC_DUNGEON_WIDTH      64
#define EXSEC_DUNGEON_HEIGHT     64
#define EXSEC_DUNGEON_TILES      4096     /* the chunk: work[0, 4096)           */
#define EXSEC_DUNGEON_WORK_BYTES 12288    /* chunk + CA buffer + prune's stack  */

enum {
    EXSEC_TILE_WALL  = 0,
    EXSEC_TILE_FLOOR = 1,
    EXSEC_TILE_DOOR  = 2,
    EXSEC_TILE_TRAP  = 3,
};

/* The seed: mix(world + index * GAMMA). A bijection of index for one world. */
uint64_t exs_fig_dungeon_seed(uint64_t world, uint64_t index);
/* The splitmix64 finaliser. mix(0x9E3779B97F4A7C15) == 0xE220A8397B1DCDAF. */
uint64_t exs_fig_dungeon_mix(uint64_t z);

/* Fills work[0, 4096). stats[0..2] = stream draws, boundary tiles roughened,
 * floor tiles pruned. Returns the walkable tiles. Reads nothing it did not
 * write first: the caller owes it neither zeroed storage nor a previous chunk. */
uint64_t exs_fig_dungeon_chunk(unsigned char *work, unsigned char *stats, uint64_t seed);

/* Floor 4-connected to the floor tile `start` stays; the rest becomes wall.
 * Returns the tiles pruned, or 4097 (touching nothing) if `start` is not floor. */
uint64_t exs_fig_dungeon_prune(unsigned char *work, uint64_t start);

/* CRC-32/ISO-HDLC (zlib's crc32) of work[0, 4096), in the low 32 bits. */
uint64_t exs_fig_dungeon_crc32(unsigned char *work);

/* The unit's one import. A trap inside Exsecutor code -- an overflow, a failed
 * bounds check -- lands here with its abort kind. The host check and the ROM
 * each define it. */
_Noreturn void exsrt_abortus(unsigned kind);

#endif /* EXSEC_DUNGEON_H */
