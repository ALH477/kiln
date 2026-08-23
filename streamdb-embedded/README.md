<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
# streamdb-embedded

A read-only [StreamDB](https://github.com/ALH477/DeMoD-StreamDB) v3 reader for
machines with no operating system — Nintendo 64 and similar. Files written by
the upstream Rust or C editions are read unmodified; this writes none.

It gives the engine a single-file, CRC-checked, suffix-indexed asset container
that can be read straight out of ROM:

```c
streamdb_emb_t db;
streamdb_emb_open(&db, &io, arena, arena_size);

size_t len = sizeof(buf);
streamdb_emb_get(&db, "models/player.t3dm", 18, buf, &len);

/* every model in the database, without a scan */
streamdb_emb_find_suffix(&db, ".t3dm", 5, on_model, NULL);
```

## Why not just port the upstream C edition

Three structural reasons, none of them incidental:

1. **It needs an OS.** pthreads (a recursive mutex plus a background auto-flush
   thread), `flock()`, `fsync()`/`fdatasync()`, and stdio `FILE*`. libdragon has
   none of these.
2. **Its trie node is `TrieNode *children[256]`** — 1045 bytes per node with
   32-bit pointers. Measured on a 200-asset database (2736 nodes) that is
   **2.73 MB of index on a console with 4 MB of RAM total**. The layout here is
   25 bytes per node: **66.8 KB, a 41.8× reduction**. Fine on a desktop, fatal
   here.
3. **Writing presumes a mutable, seekable, fsync-able file.** A ROM is none of
   those.

So this is the same format with a different memory strategy, not a port.

## Design

- **Flat trie.** Nodes live in one array; a node's children occupy a contiguous
  index range with their key bytes in a parallel array, ascending — so a child
  lookup is a small binary search and a descent touches few cache lines. That
  matters on a VR4300 with 8 KB of D-cache.
- **No `malloc`.** The caller supplies an arena. `streamdb_emb_probe()` reports
  the size needed, so a game finds out at boot rather than mid-level.
- **Two-ended arena.** Persistent structures grow up from the bottom; the trie
  and index blobs are scratch, taken from the top and released after parsing.
  On the 200-asset database that drops the resident arena from 135 KB to 75 KB.
- **Endian-neutral.** The v3 format is little-endian and the N64 is big-endian;
  every integer read goes through byte-wise helpers, so no configuration.
- **Pluggable I/O.** `streamdb_emb_io_t` has `read` and `size`. Backends ship
  for libdragon DFS (on-console) and host stdio (tests and tooling).

## The one thing the format spec does not say

The index entry's `offset` field addresses the **payload**, not the record
header — upstream writes `e.offset = next_offset + 8` after emitting the 8-byte
`size`+`crc` header. The format comment says "Document record: size u32 + CRC32
u32 + payload", which reads the other way. Getting it wrong fails on every
document with a size mismatch, and cost a debugging round here. The header is
at `offset - 8`.

## Testing

`nix flake check` runs `checks.streamdb`, which builds the **upstream C writer**
and this reader, packs a 120-asset corpus, and verifies:

- every document reads back byte-exact with its CRC verified;
- suffix search returns the right counts;
- a truncated database is rejected, not crashed on;
- a single flipped payload byte produces exactly one checksum mismatch.

Running against the real writer is the point: this parses a third-party binary
format, and on the N64 a bad parse tells you nothing. That check is what caught
the payload-offset issue above.

## Licence

LGPL-2.1-or-later, matching the upstream C edition whose format it implements.
