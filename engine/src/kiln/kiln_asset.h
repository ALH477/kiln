/* SPDX-License-Identifier: MIT
 *
 * kiln_asset.h — the runtime asset layer.
 *
 * Closes the gap `CLAUDE.md`'s "Not yet built" section used to call out: the
 * build side of the pipeline (nix/assets.nix: mkModel/mkSprite/mkSound/...)
 * was already there, but nothing in libfigulina actually opened a StreamDB at
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
 * Same pattern as fig_actor_system_init and streamdb-embedded itself.
 *
 * ── No cache ───────────────────────────────────────────────────────────
 * fig_asset_model and fig_asset_sprite malloc and return; the caller holds
 * the pointer. The demo ROM loads each asset once before the loop, exactly
 * as examples/assets-demo already does with raw t3d_model_load. A bounded
 * cache table is a one-day follow-up if an actor type ever needs on-demand
 * loading; not needed now.
 *
 * ── fig_asset_model needs t3d_model_load_buf ──────────────────────────
 * Tiny3D upstream only exposes t3d_model_load(path), which calls
 * asset_load(path, &size) and patches the returned buffer in place. There is
 * no in-memory variant, so loading a .t3dm out of a StreamDB payload —
 * already in RAM, no path — needs the patched t3d_model_load_buf that this
 * repo carries at nix/patches/tiny3d-load-buf.patch. See CLAUDE.md's Phase
 * C notes.
 *
 * ── fig_asset_wav64 is still not here, and no longer needs to be ─────
 * This used to read: "libdragon's wav64_open(wav, fn) is path-only with no
 * in-memory variant [...] It lands when a wav64_open_buf lands upstream."
 * That was the wrong remedy for the right observation, and AUDIO_REVIEW.md's
 * Gap 7 recorded the same mistake.
 *
 * wav64 does not want a buffer. It wants a file descriptor and, optionally,
 * a cartridge address (wav64.c:126, :214). Both are things a FILESYSTEM can
 * supply, and a StreamDB payload is stored raw and contiguous, so it has
 * both. kiln_sdbfs.h mounts a container and `wav64_load("sdb:/...")` then
 * streams out of it with the same asynchronous DMA a loose DFS file gets.
 *
 * An in-memory variant would have been strictly worse for the case that
 * motivated it: the first asset to need this is 6,979,644 bytes of
 * orchestra, on a console with four megabytes of RAM.
 *
 * So there is no fig_asset_wav64 and there should not be — audio is not a
 * typed accessor, it is a path, and fig_sdbfs makes the path work.
 */
#ifndef FIG_ASSET_H
#define FIG_ASSET_H


/* The prefix migration train (docs/NAMING.md section 9 step 2). Pulled in by
 * every public header (a quoted include, so it resolves both in this tree and
 * in the installed include/kiln prefix) rather than force-included by
 * kiln-inst.mk, because a
 * force-include only reaches builds that include that file — a Nix check or a
 * host build compiling a downstream's sources directly never saw it, and
 * PetaByte-Madness' pm-cine check is what proved that. Deleting the train is
 * still a scripted one-line removal from these headers plus the file itself.
 */
#include "kiln_compat.h"

#include <libdragon.h>
#include <t3d/t3dmodel.h>

#include <streamdb/streamdb_embedded.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FigAsset FigAsset;

/** Open a StreamDB file from a DFS path (e.g. "rom:/assets.streamdb").
 *
 *  The arena must outlive the handle. Size it with fig_asset_probe_size on
 *  the same path, or pass a buffer larger than that — the reader uses what
 *  it needs. Returns NULL on any open failure; the engine's assertf is not
 *  used here because a missing asset DB at boot is a content fact (wrong
 *  ROM) not a programming error, and the caller may want to fall back to
 *  loose-DFS assets rather than abort. */
FigAsset *fig_asset_open(const char *dfs_path, void *arena, size_t arena_size);

/** Close and release the arena-backed reader. Does NOT free assets returned
 *  by fig_asset_model / fig_asset_sprite — those are caller-owned. */
void fig_asset_close(FigAsset *db);

/** How much arena fig_asset_open would need for this DB. 0 on probe
 *  failure; the caller should treat 0 as an error rather than pass it back
 *  as an arena size. */
size_t fig_asset_probe_size(const char *dfs_path);

/** Document count. */
uint32_t fig_asset_count(const FigAsset *db);

/** Payload size in bytes, or 0 if the key is absent. Lets a caller size a
 *  buffer at boot rather than guessing. */
size_t fig_asset_size(const FigAsset *db, const char *key, size_t key_len);

/** Arena bytes the open reader holds for its index and trie — the persistent
 *  part of what fig_asset_probe_size sized. Against the arena the caller
 *  allocated, this is the headroom a HUD gauge can show. 0 for NULL. */
size_t fig_asset_arena_used(const FigAsset *db);

/** Boolean key presence. */
int fig_asset_exists(const FigAsset *db, const char *key, size_t key_len);

/** CRC-verified read into a caller buffer. `*len` is the buffer size on
 *  entry and the payload length on return. Returns STREAMDB_EMB_OK on
 *  success; see streamdb_embedded.h for the other result codes. */
int fig_asset_load(const FigAsset *db,
                   const char *key, size_t key_len,
                   void *buf, size_t *len);

/** Suffix-indexed scan: visits every document whose key ends in `suffix`,
 *  in trie order. Returns the number of matches visited. Passthrough to
 *  streamdb_emb_find_suffix — the reverse trie is what makes this O(suffix
 *  + matches) rather than a scan. */
int fig_asset_find_suffix(const FigAsset *db,
                          const char *suffix, size_t suffix_len,
                          int (*cb)(const streamdb_emb_doc_t *doc, void *user),
                          void *user);

/** The underlying reader, for code that needs the streamdb-embedded API
 *  directly rather than the typed accessors above — kiln_sdbfs.c is the
 *  reason this exists, because a filesystem has to do ranged reads and ask
 *  for a document's ROM address, and neither belongs on this interface.
 *  Borrowed, never owned; NULL for a NULL or unopened handle. */
streamdb_emb_t *fig_asset_reader(FigAsset *db);

/** Load and parse a sprite out of the DB. The buffer is malloc'd inside and
 *  ownership is transferred to the caller: free with plain `sprite_free`.
 *  (Mirrors libdragon's `sprite_load` contract — see kiln_asset.c for the
 *  ownership-transfer detail.) Returns NULL if the key is missing or the
 *  payload doesn't parse. */
sprite_t *fig_asset_sprite(const FigAsset *db, const char *key, size_t key_len);

/** Load and parse a Tiny3D model out of the DB. Requires the patched
 *  t3d_model_load_buf (nix/patches/tiny3d-load-buf.patch). The buffer is
 *  malloc'd inside and the T3DModel points into it; the model pointer IS
 *  the buffer pointer (same shape as upstream t3d_model_load), so freeing
 *  with `t3d_model_free` frees both. Returns NULL if the key is missing or
 *  the payload doesn't parse. */
T3DModel *fig_asset_model(const FigAsset *db, const char *key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* FIG_ASSET_H */