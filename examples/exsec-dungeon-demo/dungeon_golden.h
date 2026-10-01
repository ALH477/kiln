/* SPDX-License-Identifier: MIT
 *
 * dungeon_golden.h -- four chunks the ROM re-derives at boot and checks.
 *
 * NOT hand-written. Each row is one record of Exsecutor's
 * tests/programs/dungeon/expected.out, which prototypes/dungeon_oracle.py wrote
 * (Python bigints, zlib's crc32), and which the Exsecutor compiler's reference
 * backend, four C builds and a big-endian mips64 run all reproduce byte for
 * byte. The command that produced these lines is in PROVENANCE.md, and
 * nix/checks/exsec-dungeon-parity.nix checks every field below against the
 * committed copy of that file, so a hand edit here fails the build.
 *
 * world 0xDEADBEEFCAFEF00D, indices 0, 1, 64 and 244,140,624,999 -- the last
 * chunk of the first petabyte (10^15 / 4096 chunks, index 0 to 244,140,624,999).
 */
#ifndef DUNGEON_GOLDEN_H
#define DUNGEON_GOLDEN_H

#include <stdint.h>

typedef struct {
    uint64_t world, index, seed;
    uint16_t walkable, draws, boundary, pruned;
    uint32_t crc;
} DungeonGolden;

#define DUNGEON_GOLDEN_COUNT 4

static const DungeonGolden DUNGEON_GOLDEN[DUNGEON_GOLDEN_COUNT] = {
    { UINT64_C(0xDEADBEEFCAFEF00D), UINT64_C(0), UINT64_C(0x19104AE2406D51C3), 1793, 1242, 1152, 0, UINT32_C(0x4A1D1A71) },
    { UINT64_C(0xDEADBEEFCAFEF00D), UINT64_C(1), UINT64_C(0x901D4F652FB472CB), 1445, 1053, 963, 0, UINT32_C(0x0F2413D0) },
    { UINT64_C(0xDEADBEEFCAFEF00D), UINT64_C(64), UINT64_C(0x75E68A96140B3EFF), 1568, 1113, 1023, 0, UINT32_C(0xD8ECF3C3) },
    { UINT64_C(0xDEADBEEFCAFEF00D), UINT64_C(244140624999), UINT64_C(0xE08A9358E42D3080), 1281, 1064, 974, 0, UINT32_C(0xF6ABC1E2) },
};

#endif /* DUNGEON_GOLDEN_H */
