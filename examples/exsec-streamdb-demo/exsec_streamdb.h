// SPDX-License-Identifier: MIT
//
// exsec_streamdb.h -- the C face of `lector_streamdb.gen.c`, the StreamDB v3
// reader written in Exsecutor and emitted by `exsc --emitte c --hospes
// mips64-none-o64` (PROVENANCE.md has the commit, the command and the digest).
//
// HAND-WRITTEN, and checked against the generated file rather than trusted.
// `exsec_proto_check.c` includes this header and then the generated unit
// itself, so a prototype here that disagrees with a definition there is a
// conflicting-types compile ERROR in the ROM build -- instead of the silent,
// cross-object undefined behaviour a mismatched prototype is in C.
//
// WHAT IS NOT GUESSED. Every offset below is a `@transitus` field, and a
// `@transitus` struct's layout is its WIRE layout by the language's definition
// (spec section 5.2: packed, alignment 1, byte order in the type) -- so these
// are the StreamDB v3 format's own offsets, not a compiler's choice. The one
// non-wire aggregate the reader fills, `Arbor`, is never read here: its
// `numerus` comes back as `exs_arbor_percurre`'s return value.

#pragma once

#include <stdint.h>

// mips64-none-o64: 32-bit addresses, and so a 32-bit `mensura`. The generated
// unit asserts the same thing; this catches a header used for the wrong row.
_Static_assert(sizeof(void *) == 4, "exsec_streamdb.h: the o64 row has 32-bit addresses");

// ---- sizes, from examples/streamdb/lector_streamdb.exsc --------------------

// `acies<u8, 65536>`: the container buffer every entry point takes.
#define EXSEC_CAPACITY        65536u
// `acies<u8, 1024>`: a key buffer. Pass the full 1024 bytes, not the key's.
#define EXSEC_CLAVIS_MAXIMA    1024u
// `Caput` (128) and `Indicium` (32), both `@transitus`.
#define EXSEC_CAPUT_BYTES       128u
#define EXSEC_INDICIUM_BYTES     32u
// `Arbor` at a 32-bit `mensura`, derived from its declaration:
//   numerus  mensura               4
//   primus   acies<mensura, 2048>  8,192
//   numeri   acies<mensura, 2048>  8,192
//   habet    acies<u8, 2048>       2,048
//   claves   acies<u8, 2048>       2,048
//   idm      acies<u64, 2048>     16,384
//   idn      acies<u64, 2048>     16,384
// The reader writes through all of it, so this must not be smaller.
#define EXSEC_ARBOR_BYTES     53252u

// ---- wire offsets ----------------------------------------------------------

// `Caput`: little-endian (`:minor`) except `signum`, which is not read here.
enum {
    EXSEC_CAPUT_ARBOR_SITUS     = 16,  // u64
    EXSEC_CAPUT_ARBOR_LONGITUDO = 24,  // u64
    EXSEC_CAPUT_ARBOR_SUMMA     = 32,  // u32
    EXSEC_CAPUT_INDEX_SITUS     = 40,  // u64
    EXSEC_CAPUT_INDEX_LONGITUDO = 48,  // u64
    EXSEC_CAPUT_INDEX_SUMMA     = 56,  // u32
};

// `Indicium`: `idm`/`idn` are big-endian and not read here; the rest little.
enum {
    EXSEC_INDICIUM_SITUS     = 16,     // u64 -- the PAYLOAD offset; the 8-byte
                                       //   record header sits just before it
    EXSEC_INDICIUM_MAGNITUDO = 24,     // u32
};

static inline uint32_t exsec_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static inline uint64_t exsec_le64(const unsigned char *p)
{
    return (uint64_t)exsec_le32(p) | (uint64_t)exsec_le32(p + 4) << 32;
}

// ---- the entry points ------------------------------------------------------
// Library mode (docs/design/c-backend.md D1): every integer is `uint64_t`,
// every address `unsigned char *`, and an aggregate result arrives through a
// hidden FIRST pointer argument the caller supplies.

// The one symbol the generated unit imports. main.c supplies it.
_Noreturn void exsrt_abortus(unsigned kind);

// The newest valid header slot's offset, or >= 256 if no commit is readable.
uint64_t exs_caput_elige(unsigned char *p0, uint64_t p1);
// `Caput` at slot `p2` into the 128 bytes at `p0`.
void exs_caput_lege(unsigned char *p0, unsigned char *p1, uint64_t p2);
// CRC-32 of `p2` bytes at `p1`.
uint64_t exs_redundantia32(unsigned char *p0, uint64_t p1, uint64_t p2);
// The index blob's document count.
uint64_t exs_indicem_numera(unsigned char *p0, uint64_t p1, uint64_t p2);
// 0 when the index is well-formed against a container of length `p3`.
uint64_t exs_indicem_proba(unsigned char *p0, uint64_t p1, uint64_t p2, uint64_t p3);
// Fills the caller's ZEROED `Arbor` at `p0` through a mutable borrow and
// returns its node count -- 0 for a malformed trie, in which case `p0` may be
// partly written and must not be read. (ADR 0016: 20,680 bytes of frame.)
uint64_t exs_arbor_percurre(unsigned char *p0, unsigned char *p1, uint64_t p2, uint64_t p3);
// The index position of key `p4` (length `p5`), or >= `p3` if absent.
uint64_t exs_documentum_quaere(unsigned char *p0, unsigned char *p1, uint64_t p2,
                               uint64_t p3, unsigned char *p4, uint64_t p5);
// The `Indicium` at offset `p2` into the 32 bytes at `p0`.
void exs_indicium_lege(unsigned char *p0, unsigned char *p1, uint64_t p2);
// 0 ok, 2 checksum mismatch, 3 malformed record.
uint64_t exs_documentum_proba(unsigned char *p0, uint64_t p1, unsigned char *p2);
