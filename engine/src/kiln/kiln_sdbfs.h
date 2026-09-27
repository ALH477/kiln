/* SPDX-License-Identifier: MIT
 *
 * kiln_sdbfs.h — a StreamDB container, mounted as a filesystem.
 *
 * ── The gap this closes, and why it did not need what it said it needed ─
 * kiln_asset.h has carried this caveat since it was written:
 *
 *     fig_asset_wav64 is NOT provided here: libdragon's wav64_open(wav, fn)
 *     is path-only with no in-memory variant [...] It lands when a
 *     wav64_open_buf lands upstream.
 *
 * and AUDIO_REVIEW.md records the same thing as Gap 7. Both are wrong about
 * the remedy. wav64 does not want a buffer. Reading its open path:
 *
 *   * wav64.c:126  — `must_open(file_name)`, an ordinary POSIX open through
 *                    newlib, so ANY attached filesystem serves it;
 *   * wav64.c:214  — `ioctl(fd, IODFS_GET_ROM_BASE, &wav->st->rom_base)`;
 *   * wav64.c:93   — `wave.async_read = rom_base != 0`;
 *   * wav64_vadpcm.c:241 — with rom_base set, samples come off the cartridge
 *                    by `dma_read_async` and never touch our code at all;
 *                    without it, by `lseek` + `read` on the fd.
 *
 * So the thing wav64 actually wants from a filesystem is a file descriptor
 * and, optionally, an address. A StreamDB document can supply both, because
 * its payload is stored raw and contiguous (streamdb_emb_read is a single
 * io.read with no decompression step), which makes it a real byte range at a
 * real address inside a container that is itself a contiguous file in ROM.
 *
 * Mounting the container is therefore strictly better than the in-memory
 * variant that was being waited for: an in-RAM wav64 would cost 6.66 MB of
 * RDRAM for one orchestral cue on a 4 MB machine, where this costs a file
 * handle and streams off the cartridge exactly as a loose DFS wav64 does.
 *
 * ── And it is not only audio ────────────────────────────────────────────
 * asset.c:171 and compress/ringbuf.c:121 ask the same IODFS_GET_ROM_BASE
 * question. So sprite_load, the asset decompressors and anything else built
 * on libdragon's loaders get the container and its fast path too, through the
 * same mount, with no per-loader work. kiln_asset.h's typed accessors remain
 * the right call for game data read whole; this is for the streaming case and
 * for loaders that will only ever take a path.
 *
 * ── Paths are keys, unchanged ───────────────────────────────────────────
 * system.c:1042 hands `open` the path with the prefix already removed, so
 * "sdb:/music/mars.wav64" arrives as "music/mars.wav64" — byte-identical to
 * the StreamDB key mkStreamdb wrote. That is not a coincidence worth relying
 * on silently: nix/assets.nix's mkStreamdb documents that a key "is the path
 * the file would have had in loose DFS, so an asset can move between the two
 * containers with no code change at the call site", and this is the runtime
 * half of that promise. No translation happens here, deliberately, because a
 * translation is a place the two halves could drift.
 *
 * ── One mount, like DFS ─────────────────────────────────────────────────
 * libdragon's filesystem_t callbacks carry no user context — `open` is
 * `void *(*)(char *name, int flags)` and nothing more — so the mounted
 * container lives in a file-static, exactly as DragonFS keeps its own state.
 * A second mount is refused rather than silently replacing the first.
 *
 * That file-static open table is also why this filesystem declares itself NOT
 * thread safe: the slot allocator is a scan-and-claim, so the system has to
 * hold a mutex across it. See the filesystem_t initialiser for the whole
 * reasoning, including why system.h's remark about read-only filesystems is a
 * warning rather than permission.
 */
#ifndef FIG_SDBFS_H
#define FIG_SDBFS_H


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

#include "kiln_asset.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FIG_SDBFS_MAX_OPEN
/** Concurrently open files. Four covers the realistic worst case — a music
 *  stream, an ambience bed and a loader mid-flight — and each slot is small.
 *  Raise it here if a game opens more; running out returns EMFILE rather than
 *  corrupting anything. */
#define FIG_SDBFS_MAX_OPEN 4
#endif

/** Mount `db` at `prefix` (which must end in ":/", e.g. "sdb:/").
 *
 *  `db` is borrowed, not owned: it must outlive the mount, and closing it
 *  while files are open is a caller bug this cannot detect. Returns 0 on
 *  success, -1 if something is already mounted, the prefix is malformed, or
 *  libdragon refused the attach. */
int fig_sdbfs_mount(const char *prefix, FigAsset *db);

/** Unmount. Safe when nothing is mounted. Open handles are invalidated. */
void fig_sdbfs_unmount(void);

/** Whether the ROM fast path is available for `key` — that is, whether a
 *  loader opening it will get a non-zero IODFS_GET_ROM_BASE and stream by
 *  DMA rather than by byte copy.
 *
 *  WORTH CHECKING, BECAUSE FAILURE HERE IS SILENT. dfs_rom_addr guarantees
 *  the container is 2-byte aligned but says nothing about a document inside
 *  it, and libdragon's wav64 tests `!((dest ^ pi_addr) & 1)` before using
 *  DMA — an odd payload offset simply drops to the copy path with no error
 *  anywhere. A ROM that expected to stream a 6.66 MB cue by DMA and is
 *  quietly memcpy-ing it instead will sound fine in an emulator and starve on
 *  hardware, so a debug overlay should show this.
 *
 *  Returns 1 for a DMA-capable key, 0 otherwise (absent key, no mount, host
 *  backend, or odd alignment). */
int fig_sdbfs_key_is_dma(const char *key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* FIG_SDBFS_H */
