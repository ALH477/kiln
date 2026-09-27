/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/system.h — libdragon's filesystem-attach layer, on the
 * host.
 *
 * ── Why this exists ───────────────────────────────────────────────────
 * fig_sdbfs mounts a StreamDB container as a filesystem at `sdb:/`, and it
 * does that the way libdragon intends: it fills in a `filesystem_t` vtable and
 * hands it to attach_filesystem, so every ordinary loader — wav64, t3d_model,
 * sprite — reaches the container through the same dfs_open every other asset
 * uses. That is the whole point of the design, and it is why the module needs
 * no special case anywhere in the engine.
 *
 * Without this header, fig_sdbfs is simply not host-buildable and a game that
 * streams anything out of a container cannot run natively at all. With it, the
 * module compiles and runs UNMODIFIED on both targets, which is the property
 * plat/host exists to protect.
 *
 * ── The rule this file follows ────────────────────────────────────────
 * plat/host/include/libdragon.h states it: "Copy the real definition; do not
 * approximate it." The struct below is libdragon's, member for member and in
 * order, including the members Kiln never fills in — an engine that assigned
 * to the wrong designated initialiser because the host's layout was shorter
 * would be a bug that only appears on console.
 */
#ifndef FIG_HOST_SYSTEM_H
#define FIG_HOST_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

/* libdragon's directory entry. Declared because filesystem_t's findfirst and
 * findnext name it; nothing in Kiln implements either. */
#ifndef FIG_HOST_DIR_T
#define FIG_HOST_DIR_T
typedef struct {
    int  d_type;
    char d_name[256];
    size_t d_size;
    uint32_t d_cookie;
} dir_t;
#endif

/** Verbatim from libdragon's system.h. */
typedef struct
{
    bool thread_safe;
    void *(*open)( char *name, int flags );
    int (*fstat)( void *file, struct stat *st );
    int (*stat)( char *name, struct stat *st );
    int (*lseek)( void *file, int ptr, int dir );
    int (*read)( void *file, uint8_t *ptr, int len );
    int (*write)( void *file, uint8_t *ptr, int len );
    int (*close)( void *file );
    int (*unlink)( char *name );
    int (*findfirst)( char *path, dir_t *dir );
    int (*findnext)( dir_t *dir );
    int (*findnext2)( const char *path, dir_t *dir );
    int (*ftruncate)( void *file, int length );
    int (*mkdir)( char *path, mode_t mode );
    int (*ioctl)(void *file, unsigned long cmd, void *argp);
    int (*utimes)(const char *path, const struct timeval times[2]);
} filesystem_t;

/** Hook a filesystem in at `prefix` (e.g. "sdb:/"). 0 on success.
 *
 *  As on console, the prefix is STRIPPED before the filesystem's own `open`
 *  sees the name — libdragon does that in system.c and fig_sdbfs's open
 *  documents that it relies on it, so a host that passed the whole path would
 *  look up "sdb:/music/x" as a key and find nothing. */
int attach_filesystem( const char * const prefix, filesystem_t *filesystem );

/** Remove it again. 0 on success. */
int detach_filesystem( const char * const prefix );

#endif /* FIG_HOST_SYSTEM_H */
