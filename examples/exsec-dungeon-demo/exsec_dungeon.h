/* SPDX-License-Identifier: MIT
 *
 * The C face of examples/dungeon/furor_petabytorum.exsc and dungeon.exsc, emitted by the Exsecutor
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

/* furor_petabytorum.exsc -- the seed module of PetaByte Madness.
 *
 * semina_furore: the seed of chunk `index` in world `orbis`, mix(orbis + index *
 * GAMMA). A bijection of index for one orbis, so no two chunks of a world share
 * a seed. desemina_furore is its inverse -- NOTE THE ORDER, seed first, which is
 * what spec 3.5's `de-` prescribes -- so desemina(semina(o, i), o) == i.
 * misce/demisce are the splitmix64 finaliser and its inverse;
 * misce(0x9E3779B97F4A7C15) == 0xE220A8397B1DCDAF. */
uint64_t exs_semina_furore(uint64_t orbis, uint64_t index);
uint64_t exs_desemina_furore(uint64_t semen, uint64_t orbis);
uint64_t exs_misce_furore(uint64_t z);
uint64_t exs_demisce_furore(uint64_t z);

/* The same, addressed by chunk COORDINATES: the index is (y << 32) | x, so every
 * chunk of a 2^32 x 2^32 plane has its own seed. Coordinates are UNSIGNED. A
 * signed chunk coordinate is passed as its uint32_t cast (-1 is 0xFFFFFFFF),
 * which is a bijection; they are not int32_t so that no caller can hand the unit
 * a sign-extended value it does not expect (the emitted C holds an iN
 * sign-extended in its uint64_t carrier; a uN is zero-extended, which is what
 * a uint32_t widens to). exsec_semina_plano below does the cast for you.
 * 2^19 chunks a side is exactly one pebibyte. */
uint64_t exs_semina_plano(uint64_t orbis, uint64_t x, uint64_t y);
/* xy[0] = x, xy[1] = y of the chunk whose seed is `semen`; pass a uint32_t[2]. */
uint64_t exs_desemina_plano(uint64_t semen, uint64_t orbis, unsigned char *xy);

/* Signed convenience wrappers. */
static inline uint64_t exsec_semina_plano(uint64_t orbis, int32_t x, int32_t y)
{
    return exs_semina_plano(orbis, (uint32_t)x, (uint32_t)y);
}
static inline void exsec_desemina_plano(uint64_t semen, uint64_t orbis, int32_t *x, int32_t *y)
{
    uint32_t xy[2];
    exs_desemina_plano(semen, orbis, (unsigned char *)xy);
    *x = (int32_t)xy[0];
    *y = (int32_t)xy[1];
}


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
