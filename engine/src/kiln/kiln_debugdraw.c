/* SPDX-License-Identifier: MIT
 *
 * kiln_debugdraw.c — see kiln_debugdraw.h.
 */
#include "kiln_debugdraw.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include "kiln_gui.h"

/* How far in front of the camera a clipped segment's new endpoint is placed.
 *
 * In world units, and deliberately not the scene's `near_z`: kiln_scene_project
 * does not use near_z either (it divides by view-space Z directly), so
 * clipping to near_z would put the endpoint somewhere the projection does not
 * treat as special and produce a visible kink at the plane. This is a small
 * positive epsilon whose only job is to keep the divide well away from zero.
 * At this project's 64 units per metre it is 1.5 cm, far below one pixel at
 * any distance a debug line is useful. */
#define DD_NEAR 1.0f

static const KilnScene *g_scene;
static int g_w, g_h;
static uint16_t g_drawn, g_clipped;

void kiln_dd_begin(const KilnScene *scene, int screen_w, int screen_h)
{
    g_scene = scene;
    g_w = screen_w;
    g_h = screen_h;
    g_drawn = 0;
    g_clipped = 0;
}

void kiln_dd_end(void)
{
    /* Cleared rather than left dangling: the scene is typically a local in
     * main()'s frame loop, and a primitive called after the block (a stray
     * call from a different screen's draw path, say) would otherwise read a
     * pointer that is merely still in scope. Every entry point below returns
     * early on a NULL scene, so a misplaced call is a no-op. */
    g_scene = NULL;
}

uint16_t kiln_dd_drawn(void)   { return g_drawn; }
uint16_t kiln_dd_clipped(void) { return g_clipped; }

static inline fm_vec3_t lerp3(fm_vec3_t a, fm_vec3_t b, float t)
{
    fm_vec3_t o;
    for (int i = 0; i < 3; i++) o.v[i] = a.v[i] + (b.v[i] - a.v[i]) * t;
    return o;
}

/* ── 2D clip to the viewport ────────────────────────────────────────────
 * Liang-Barsky against the screen rectangle. This is not cosmetic: a line
 * whose endpoint projects to a few hundred thousand pixels off screen — which
 * happens routinely for a point just past the near plane — reaches rdpq as a
 * triangle whose coordinates overflow the RDP's fixed-point vertex format, and
 * the result is not an off-screen line but a garbage triangle drawn ACROSS the
 * frame. Clamping the endpoints instead would keep them in range but change
 * the line's slope, drawing a line that is not the one asked for; a real clip
 * keeps the slope and is barely more code.
 *
 * Returns 0 when the segment is entirely outside. */
static int clip2d(float *x0, float *y0, float *x1, float *y1, float w, float h)
{
    float t0 = 0.0f, t1 = 1.0f;
    const float dx = *x1 - *x0, dy = *y1 - *y0;

    /* p = -dx, q = x0 - xmin  ... for each of the four half-planes. */
    const float p[4] = { -dx,  dx, -dy,  dy };
    const float q[4] = { *x0 - 0.0f, w - *x0, *y0 - 0.0f, h - *y0 };

    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return 0;   /* parallel and outside */
            continue;
        }
        const float r = q[i] / p[i];
        if (p[i] < 0.0f) {
            if (r > t1) return 0;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return 0;
            if (r < t1) t1 = r;
        }
    }

    const float nx0 = *x0 + t0 * dx, ny0 = *y0 + t0 * dy;
    const float nx1 = *x0 + t1 * dx, ny1 = *y0 + t1 * dy;
    *x0 = nx0; *y0 = ny0; *x1 = nx1; *y1 = ny1;
    return 1;
}

void kiln_dd_line(fm_vec3_t a, fm_vec3_t b, color_t c)
{
    if (!g_scene) return;

    /* ── Camera-plane clip, in view space, before projecting ────────────
     * kiln_scene_project's behind-camera branch pushes a point far out along
     * its off-axis direction and hands the clamp-or-cull decision back (see
     * its comment). For a segment neither answer is right: culling loses every
     * line that merely starts behind the camera — which for a wireframe box
     * the camera is standing inside is all twelve of them — and clamping draws
     * a line to a point that is not on the segment. Interpolating the crossing
     * is the only option that draws the truth. */
    float za = kiln_scene_depth(g_scene, a);
    float zb = kiln_scene_depth(g_scene, b);

    if (za < DD_NEAR && zb < DD_NEAR) { g_clipped++; return; }
    if (za < DD_NEAR) a = lerp3(a, b, (DD_NEAR - za) / (zb - za));
    else if (zb < DD_NEAR) b = lerp3(b, a, (DD_NEAR - zb) / (za - zb));

    int ax, ay, bx, by;
    kiln_scene_project(g_scene, a, g_w, g_h, &ax, &ay);
    kiln_scene_project(g_scene, b, g_w, g_h, &bx, &by);

    float fx0 = (float)ax, fy0 = (float)ay, fx1 = (float)bx, fy1 = (float)by;
    if (!clip2d(&fx0, &fy0, &fx1, &fy1, (float)g_w, (float)g_h)) {
        g_clipped++;
        return;
    }

    kiln_gui_line((int)fx0, (int)fy0, (int)fx1, (int)fy1, 1, c);
    g_drawn++;
}

void kiln_dd_aabb(fm_vec3_t mins, fm_vec3_t maxs, color_t c)
{
    if (!g_scene) return;

    /* Eight corners, indexed so bit 0 = X, bit 1 = Y, bit 2 = Z. The edge
     * table below then reads as "the pairs differing in exactly one bit",
     * which is what a box's twelve edges are — cheaper to check by eye than
     * twelve hand-written coordinate triples, and this is a debug facility
     * whose own correctness nobody will test. */
    fm_vec3_t v[8];
    for (int i = 0; i < 8; i++) {
        v[i].v[0] = (i & 1) ? maxs.v[0] : mins.v[0];
        v[i].v[1] = (i & 2) ? maxs.v[1] : mins.v[1];
        v[i].v[2] = (i & 4) ? maxs.v[2] : mins.v[2];
    }
    static const uint8_t EDGES[12][2] = {
        { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },   /* along X */
        { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },   /* along Y */
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },   /* along Z */
    };
    for (int e = 0; e < 12; e++)
        kiln_dd_line(v[EDGES[e][0]], v[EDGES[e][1]], c);
}

void kiln_dd_box(fm_vec3_t centre, fm_vec3_t half, color_t c)
{
    fm_vec3_t mins, maxs;
    for (int i = 0; i < 3; i++) {
        mins.v[i] = centre.v[i] - half.v[i];
        maxs.v[i] = centre.v[i] + half.v[i];
    }
    kiln_dd_aabb(mins, maxs, c);
}

void kiln_dd_axes(fm_vec3_t origin, float len)
{
    if (!g_scene) return;

    /* ENGINE axes, not Blender's — see the header. R/G/B for X/Y/Z is the
     * near-universal convention and tools/blender/models.py's `axes` test
     * model already colours itself by the engine axis, so a capture of that
     * model and this gizmo agree. */
    for (int i = 0; i < 3; i++) {
        fm_vec3_t tip = origin;
        tip.v[i] += len;
        const color_t col = (i == 0) ? RGBA32(255, 64, 64, 255)
                          : (i == 1) ? RGBA32(64, 255, 64, 255)
                                     : RGBA32(96, 128, 255, 255);
        kiln_dd_line(origin, tip, col);
    }
}

void kiln_dd_frustum(fm_vec3_t eye, fm_vec3_t look, float fov_deg, float aspect,
                    float near_z, float far_z, color_t c)
{
    if (!g_scene) return;
    if (!(far_z > near_z) || !(near_z > 0.0f)) return;

    /* Build the camera basis the same way kiln_scene_update does — forward from
     * eye to look, right from forward x world-up, up from right x forward. A
     * second derivation here would be a second thing to get wrong, and the
     * whole value of this primitive is that it shows the volume the RENDERER
     * is using. */
    fm_vec3_t f;
    float flen = 0.0f;
    for (int i = 0; i < 3; i++) {
        f.v[i] = look.v[i] - eye.v[i];
        flen += f.v[i] * f.v[i];
    }
    flen = sqrtf(flen);
    if (flen <= 0.0f) return;   /* the degenerate view vector; nothing to draw */
    for (int i = 0; i < 3; i++) f.v[i] /= flen;

    /* right = forward x up_world. Degenerate when the camera looks straight
     * down, which several of PetaByte Madness' shots do (the intake's held
     * overhead is exactly that) — fall back to world +X so a top-down shot
     * still draws a frustum instead of nothing. */
    fm_vec3_t r = {{ f.v[2], 0.0f, -f.v[0] }};
    float rlen = sqrtf(r.v[0] * r.v[0] + r.v[2] * r.v[2]);
    if (rlen < 1e-4f) { r = (fm_vec3_t){{ 1.0f, 0.0f, 0.0f }}; rlen = 1.0f; }
    for (int i = 0; i < 3; i++) r.v[i] /= rlen;

    /* up = right x forward */
    const fm_vec3_t u = {{
        r.v[1] * f.v[2] - r.v[2] * f.v[1],
        r.v[2] * f.v[0] - r.v[0] * f.v[2],
        r.v[0] * f.v[1] - r.v[1] * f.v[0],
    }};

    /* KilnScene's fov_deg is the VERTICAL field of view, so the half-height is
     * what tan() gives and the half-width follows from the aspect. Getting
     * this the other way round draws a plausible frustum of the wrong shape,
     * which is the worst possible outcome for a diagnostic. */
    const float th = tanf(fov_deg * 0.5f * 0.017453293f);

    fm_vec3_t corner[8];
    for (int p = 0; p < 2; p++) {
        const float d = p ? far_z : near_z;
        const float hh = th * d, hw = hh * aspect;
        for (int q = 0; q < 4; q++) {
            const float sx = (q == 1 || q == 2) ? 1.0f : -1.0f;
            const float sy = (q >= 2) ? 1.0f : -1.0f;
            for (int i = 0; i < 3; i++)
                corner[p * 4 + q].v[i] = eye.v[i] + f.v[i] * d
                                       + r.v[i] * sx * hw + u.v[i] * sy * hh;
        }
    }

    for (int q = 0; q < 4; q++) {
        kiln_dd_line(corner[q], corner[(q + 1) & 3], c);              /* near */
        kiln_dd_line(corner[4 + q], corner[4 + ((q + 1) & 3)], c);    /* far  */
        kiln_dd_line(corner[q], corner[4 + q], c);                    /* side */
    }
    /* One diagonal per plane. Without them the two rectangles read as loops
     * floating in space; with them they read as surfaces, and "the far plane
     * passes through the temple" becomes a thing you can see rather than
     * infer. */
    kiln_dd_line(corner[0], corner[2], c);
    kiln_dd_line(corner[4], corner[6], c);
}

void kiln_dd_point(fm_vec3_t p, int px, color_t c)
{
    if (!g_scene) return;

    /* Screen-space, so the marker keeps its size at any distance. A world-space
     * cross would vanish exactly when it is most wanted — on the far object
     * you are trying to find. */
    if (kiln_scene_depth(g_scene, p) < DD_NEAR) { g_clipped++; return; }
    if (px < 1) px = 1;

    int sx, sy;
    kiln_scene_project(g_scene, p, g_w, g_h, &sx, &sy);
    if (sx < -px || sy < -px || sx > g_w + px || sy > g_h + px) {
        g_clipped++;
        return;
    }
    kiln_gui_line(sx - px, sy, sx + px, sy, 1, c);
    kiln_gui_line(sx, sy - px, sx, sy + px, 1, c);
    g_drawn++;
}

void kiln_dd_path(const fm_vec3_t *pts, int n, color_t c)
{
    if (!g_scene || !pts || n < 2) return;
    for (int i = 0; i < n - 1; i++) kiln_dd_line(pts[i], pts[i + 1], c);
}

void kiln_dd_text(fm_vec3_t p, color_t c, const char *fmt, ...)
{
    if (!g_scene) return;

    /* Behind the camera: drawn nowhere. A label clamped to a screen edge would
     * name a thing the viewer cannot see, which is worse than silence — the
     * reticle can get away with it because a reticle means "your target is
     * that way" and a label means "this is here". */
    if (kiln_scene_depth(g_scene, p) < DD_NEAR) { g_clipped++; return; }

    int sx, sy;
    kiln_scene_project(g_scene, p, g_w, g_h, &sx, &sy);
    if (sx < 0 || sy < 0 || sx > g_w || sy > g_h) { g_clipped++; return; }

    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    /* "%s" rather than passing buf as the format: buf holds already-formatted
     * text, and handing it back to kiln_gui_text as a format string would make
     * any '%' in a caller's label — an actor name, a percentage in a debug
     * string — read arguments that were never passed. */
    kiln_gui_text(sx + 3, sy - 3, c, "%s", buf);
    g_drawn++;
}

