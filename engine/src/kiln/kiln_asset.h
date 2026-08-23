/* SPDX-License-Identifier: MIT
 *
 * kiln_asset.h — the runtime asset layer.
 *
 * Closes the gap `CLAUDE.md`'s "Not yet built" section used to call out: the
 * build side of the pipeline (nix/assets.nix: mkModel/mkSprite/mkSound/...)
 * was already there, but nothing in libkiln actually opened a StreamDB at
 * runtime. This is that runtime layer.
 *
 * ── Why a StreamDB layer at all, when DFS already exists ───────────────
 * DFS is a flat directory of loose files baked into the ROM. StreamDB is a
 * single-file, CRC-checked, suffix-indexed container read straight out of
 * ROM. Two containers, two purposes:
 *
 *   * DFS for assets the engine loaders consume by path — t3d_model_load,
 *     wav64_open, anything else that's hardcoded to a `rom:/...` string.
 *   * StreamDB for game data and anything indexed: level layouts, dialogue
 *     tables, actor params, and (with the typed accessors below) any model
 *     or sprite that benefits from a single packed container plus a
 *     suffix scan ("every .t3dm in this DB").
 *
 * ── One arena, caller-owned ────────────────────────────────────────────
 * No malloc anywhere in here. The caller passes the arena at open time,
 * sized with streamdb_emb_probe (or just give it more than that — the reader
 * uses what it needs and frees the rest back). A heap failure mid-level is
 * not recoverable on a 4 MB console, so the layer finds out at boot instead.
 * Same pattern as kiln_actor_system_init and streamdb-embedded itself.
 *
 * ── No cache ───────────────────────────────────────────────────────────
 * kiln_asset_model and kiln_asset_sprite malloc and return; the caller holds
 * the pointer. The demo ROM loads each asset once before the loop, exactly
 * as examples/assets-demo already does with raw t3d_model_load. A bounded
 * cache table is a one-day follow-up if an actor type ever needs on-demand
 * loading; not needed now.
 *
 * ── kiln_asset_model needs t3d_model_load_buf ──────────────────────────
 * Tiny3D upstream only exposes t3d_model_load(path), which calls
 * asset_load(path, &size) and patches the returned buffer in place. There is
 * no in-memory variant, so loading a .t3dm out of a StreamDB payload —
 * already in RAM, no path — needs the patched t3d_model_load_buf that this
 * repo carries at nix/patches/tiny3d-load-buf.patch. See CLAUDE.md's Phase
 * C notes.
 *
 * kiln_asset_wav64 is NOT provided here: libdragon's wav64_open(wav, fn) is
 * path-only with no in-memory variant, and DFS is read-only at runtime so
 * the obvious "copy to DFS then wav64_open" workaround is unavailable. It
 * lands when a wav64_open_buf lands upstream, same shape as the Tiny3D
 * patch.
 */
#ifndef KILN_ASSET_H
#define KILN_ASSET_H

#include <libdragon.h>
#include <t3d/t3dmodel.h>

#include <streamdb/streamdb_embedded.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KilnAsset KilnAsset;

/** Open a StreamDB file from a DFS path (e.g. "rom:/assets.streamdb").
 *
 *  The arena must outlive the handle. Size it with kiln_asset_probe_size on
 *  the same path, or pass a buffer larger than that — the reader uses what
 *  it needs. Returns NULL on any open failure; the engine's assertf is not
 *  used here because a missing asset DB at boot is a content fact (wrong
 *  ROM) not a programming error, and the caller may want to fall back to
 *  loose-DFS assets rather than abort. */
KilnAsset *kiln_asset_open(const char *dfs_path, void *arena, size_t arena_size);

/** Close and release the arena-backed reader. Does NOT free assets returned
 *  by kiln_asset_model / kiln_asset_sprite — those are caller-owned. */
void kiln_asset_close(KilnAsset *db);

/** How much arena kiln_asset_open would need for this DB. 0 on probe
 *  failure; the caller should treat 0 as an error rather than pass it back
 *  as an arena size. */
size_t kiln_asset_probe_size(const char *dfs_path);

/** Document count. */
uint32_t kiln_asset_count(const KilnAsset *db);

/** Payload size in bytes, or 0 if the key is absent. Lets a caller size a
 *  buffer at boot rather than guessing. */
size_t kiln_asset_size(const KilnAsset *db, const char *key, size_t key_len);

/** Boolean key presence. */
int kiln_asset_exists(const KilnAsset *db, const char *key, size_t key_len);

/** CRC-verified read into a caller buffer. `*len` is the buffer size on
 *  entry and the payload length on return. Returns STREAMDB_EMB_OK on
 *  success; see streamdb_embedded.h for the other result codes. */
int kiln_asset_load(const KilnAsset *db,
                   const char *key, size_t key_len,
                   void *buf, size_t *len);

/** Suffix-indexed scan: visits every document whose key ends in `suffix`,
 *  in trie order. Returns the number of matches visited. Passthrough to
 *  streamdb_emb_find_suffix — the reverse trie is what makes this O(suffix
 *  + matches) rather than a scan. */
int kiln_asset_find_suffix(const KilnAsset *db,
                          const char *suffix, size_t suffix_len,
                          int (*cb)(const streamdb_emb_doc_t *doc, void *user),
                          void *user);

/** Load and parse a sprite out of the DB. The buffer is malloc'd inside and
 *  ownership is transferred to the caller: free with plain `sprite_free`.
 *  (Mirrors libdragon's `sprite_load` contract — see kiln_asset.c for the
 *  ownership-transfer detail.) Returns NULL if the key is missing or the
 *  payload doesn't parse. */
sprite_t *kiln_asset_sprite(const KilnAsset *db, const char *key, size_t key_len);

/** Load and parse a Tiny3D model out of the DB. Requires the patched
 *  t3d_model_load_buf (nix/patches/tiny3d-load-buf.patch). The buffer is
 *  malloc'd inside and the T3DModel points into it; the model pointer IS
 *  the buffer pointer (same shape as upstream t3d_model_load), so freeing
 *  with `t3d_model_free` frees both. Returns NULL if the key is missing or
 *  the payload doesn't parse. */
T3DModel *kiln_asset_model(const KilnAsset *db, const char *key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* KILN_ASSET_H */