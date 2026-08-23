/* SPDX-License-Identifier: MIT
 *
 * host_internal.h — what host_gfx.c lends to host_t3d.c.
 *
 * The 2D and 3D passes share one framebuffer and one depth buffer, exactly as
 * they do on console, so they have to share the pixel write. This is the whole
 * of that seam. Nothing outside plat/host/src may include it: the public
 * surface is <libdragon.h>, <t3d/t3d.h> and <kiln_host.h>.
 *
 * Every check that links the host backend globs every .c in plat/host/src rather than
 * naming the files. Naming them meant three lists, and the third one broke the
 * moment host_tex.c arrived and host_t3d.c started calling into it — the same
 * drift engine/modules.mk exists to prevent, one directory over.
 */
#ifndef KILN_HOST_INTERNAL_H
#define KILN_HOST_INTERNAL_H

#include <libdragon.h>
#include <stdint.h>

int  kiln_hostfb_w(void);
int  kiln_hostfb_h(void);
int  kiln_hostfb_attached(void);

/** 2D write: honours the rdpq blender, ignores depth. */
void kiln_hostfb_put(int x, int y, color_t c);

/** 3D write. `z` is 0 (near) .. 65535 (far). `test`/`write` are the two
 *  halves of rdpq_mode_zbuf, kept separate because the engine sets them
 *  separately and a decal pass wants test-without-write. */
void kiln_hostfb_put_z(int x, int y, uint16_t z, color_t c, int test, int write);

void kiln_hostfb_clear_color(color_t c);
void kiln_hostfb_clear_depth(void);

/** The two halves of the caller's last rdpq_mode_zbuf. */
int kiln_hostfb_ztest(void);
int kiln_hostfb_zwrite(void);

/** The colour the 3D pass fogs toward, as set by rdpq_set_fog_color. Read
 *  through the same call the engine makes so the two cannot disagree. */
color_t kiln_hostfb_fog_color(void);

/** The combiner the caller last selected. The 3D pass needs it to know whether
 *  a sampled texel actually reaches the framebuffer — see host_tex.c. */
rdpq_combiner_t kiln_hostfb_combiner(void);

/** Sample the texture bound to `tile` at (s,t) in texels. Returns 0 if no
 *  texture is bound. Defined in host_tex.c. */
int kiln_hosttex_sample(int tile, float s, float t, color_t *out);

#endif /* KILN_HOST_INTERNAL_H */
