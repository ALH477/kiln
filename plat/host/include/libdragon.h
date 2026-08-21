/* SPDX-License-Identifier: MPL-2.0
 *
 * plat/host/include/libdragon.h — the host's <libdragon.h>, system tier.
 *
 * The engine includes <libdragon.h> in 36 files and, in 28 of them, wants
 * almost nothing from it: `debugf`, `assertf`, `assert`, `malloc_uncached`,
 * `color_t` and the tick counter. That is the surface below — enough that
 * those modules compile natively, UNMODIFIED, with no #ifdef anywhere in the
 * engine. Keeping the engine sources identical across both targets is the
 * whole design: see nix/checks/kiln-logic.nix's header for what a fork of the
 * sources instead of a shim would cost.
 *
 * ── What this is NOT ──────────────────────────────────────────────────
 * There is no rdpq, no Tiny3D, no mixer, no joypad and no filesystem here.
 * Modules that need those do not compile against this header, and that is the
 * point: the compiler decides which tier a module is in, so the tier list in
 * engine/modules.mk cannot quietly disagree with reality.
 *
 * ── The rule for anything added here ──────────────────────────────────
 * Copy the real definition; do not approximate it. `color_t` and
 * `resolution_t` below are libdragon's own, byte for byte, and color_t keeps
 * libdragon's `_Static_assert(sizeof(color_t) == 4)` because 31 engine
 * signatures pass it by value.
 *
 * Where a host value cannot match the console's, it is stated rather than
 * papered over — see TICKS_PER_SECOND.
 */
#ifndef KILN_HOST_LIBDRAGON_H
#define KILN_HOST_LIBDRAGON_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

/* ── diagnostics ───────────────────────────────────────────────────────
 * debugf goes to stderr rather than nowhere: several modules use it to report
 * a policy decision a caller may want to see (kiln_event's pool-full
 * eviction, kiln_cache's refcount complaints), and discarding them would make
 * "handled and reported" indistinguishable from "silently did nothing".
 *
 * assertf ABORTS, matching libdragon rather than softening it. kiln_clip uses
 * it for the caller-contract violations it refuses to guess about; a host
 * build that merely logged would run on input the console dies on, which is
 * the opposite of useful. */
#define debugf(...) fprintf(stderr, __VA_ARGS__)

#define assertf(cond, ...) do {                                             \
        if (!(cond)) {                                                      \
            fprintf(stderr, "ASSERTION FAILED: %s\n  ", #cond);             \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n  at %s:%d\n", __FILE__, __LINE__);          \
            abort();                                                        \
        }                                                                   \
    } while (0)

/* ── colour ────────────────────────────────────────────────────────────
 * Verbatim from libdragon's graphics.h, including the packed attribute and
 * the size assertion. 31 engine header signatures take a color_t by value. */
typedef struct __attribute__((packed)) {
    uint8_t r, g, b, a;
} color_t;

#ifndef __cplusplus
_Static_assert(sizeof(color_t) == 4, "invalid sizeof for color_t");
#endif

#define RGBA32(rx, gx, bx, ax) ((color_t){ .r = rx, .g = gx, .b = bx, .a = ax })

/* The constant-folding branch of libdragon's RGBA16 is a code-size
 * optimisation for the VR4300 and makes no difference to the value, so the
 * host keeps only the general form. 5-bit channels are replicated into 8 bits
 * the same way (x << 3 | x >> 3), which is what makes 31 map to 255. */
#define RGBA16(rx, gx, bx, ax) (__extension__ ({                            \
        int _r = (rx), _g = (gx), _b = (bx), _a = (ax);                     \
        (color_t){ .r = (uint8_t)((_r << 3) | (_r >> 3)),                   \
                   .g = (uint8_t)((_g << 3) | (_g >> 3)),                   \
                   .b = (uint8_t)((_b << 3) | (_b >> 3)),                   \
                   .a = (uint8_t)(_a ? 0xFF : 0) };                         \
    }))

/* ── display geometry ──────────────────────────────────────────────────
 * kiln_engine_init takes a resolution_t. Only the geometry fields are read by
 * anything in the engine; the rest of libdragon's struct is reproduced so the
 * designated initialisers below stay valid. */
typedef enum { INTERLACE_OFF, INTERLACE_HALF, INTERLACE_FULL } interlace_mode_t;

typedef struct {
    int32_t width;
    int32_t height;
    interlace_mode_t interlaced;
    bool aspect_ratio_correction;
    float overscan_margin;
} resolution_t;

#define VI_CRT_MARGIN 0.05f

/* `static const` matches libdragon, which does the same via a doxygen trick.
 * Per-translation-unit copies of a 20-byte struct are not worth an extern. */
static const resolution_t RESOLUTION_256x240 = { .width = 256, .height = 240, .interlaced = INTERLACE_OFF };
static const resolution_t RESOLUTION_320x240 = { .width = 320, .height = 240, .interlaced = INTERLACE_OFF };
static const resolution_t RESOLUTION_512x240 = { .width = 512, .height = 240, .interlaced = INTERLACE_OFF };
static const resolution_t RESOLUTION_640x240 = { .width = 640, .height = 240, .interlaced = INTERLACE_OFF };
static const resolution_t RESOLUTION_512x480 = { .width = 512, .height = 480, .interlaced = INTERLACE_HALF };
static const resolution_t RESOLUTION_640x480 = { .width = 640, .height = 480, .interlaced = INTERLACE_HALF };

/* ── uncached allocation ───────────────────────────────────────────────
 * On the console this returns memory in the uncached segment so the RSP sees
 * writes without a cache writeback. A host has one coherent view, so plain
 * malloc is not an approximation of this — it is the whole of what the
 * abstraction means off-console. The alignment is honoured because callers
 * (kiln_voxmesh's vertex arena, kiln_map's face buffers) DMA from it on
 * hardware and assume it. */
static inline void *malloc_uncached(size_t size) {
    void *p = NULL;
    if (posix_memalign(&p, 16, (size + 15) & ~(size_t)15) != 0) return NULL;
    return p;
}
static inline void *malloc_uncached_aligned(int align, size_t size) {
    void *p = NULL;
    size_t a = (size_t)align < sizeof(void *) ? sizeof(void *) : (size_t)align;
    if (posix_memalign(&p, a, (size + a - 1) & ~(a - 1)) != 0) return NULL;
    return p;
}
static inline void free_uncached(void *buf) { free(buf); }

/* ── cache coherency, which is a no-op off-console ────────────────────
 * On the VR4300 these push the CPU's view of memory out so the RSP, reading
 * over the system bus, sees it. A host has one coherent view, so there is
 * nothing to push — this is not an approximation of the operation, it is the
 * whole of what the operation means here.
 *
 * They stay as functions rather than becoming empty macros so a caller that
 * passes the wrong thing still gets a type error, and so the argument is still
 * evaluated exactly once. */
static inline void data_cache_hit_writeback(volatile const void *p, unsigned long n)
{ (void)p; (void)n; }
static inline void data_cache_hit_writeback_invalidate(volatile void *p, unsigned long n)
{ (void)p; (void)n; }
static inline void data_cache_hit_invalidate(volatile void *p, unsigned long n)
{ (void)p; (void)n; }
static inline void data_cache_writeback_invalidate_all(void) { }
static inline void inst_cache_hit_invalidate(volatile void *p, unsigned long n)
{ (void)p; (void)n; }

/* Segment translation. On console these move a pointer between the cached and
 * uncached views of the same physical memory; here there is one view, so both
 * are the identity. Kept because kiln_scratch names them explicitly. */
#define UncachedAddr(p)       (p)
#define CachedAddr(p)         (p)
#define UncachedShortAddr(p)  (p)
#define PhysicalAddr(p)       ((unsigned long)(p))

/* ── the tick counter ──────────────────────────────────────────────────
 * TICKS_READ() is COP0's count register, which ticks at half the VR4300's
 * 93.75 MHz. There is no host equivalent and pretending otherwise would make
 * kiln_prof report numbers that look like console numbers and are not, so the
 * host counter is monotonic nanoseconds scaled to the SAME rate: a duration
 * measured in ticks means the same span of wall time on both targets, while
 * an absolute tick value means nothing on either.
 *
 * What a host build therefore CANNOT tell you is what kiln_prof exists to
 * measure — the VR4300's actual cost. Wall time on a machine three orders of
 * magnitude faster is not that. Profile on hardware; see nix/faust.nix's
 * cycle-budget gate for the same caveat stated about static estimates. */
#define CPU_FREQUENCY    (93750000)
#define TICKS_PER_SECOND (CPU_FREQUENCY / 2)

static inline uint32_t kiln_host_ticks32(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return (uint32_t)((ns * TICKS_PER_SECOND) / 1000000000ull);
}

#define TICKS_READ()             kiln_host_ticks32()
#define TICKS_DISTANCE(from, to) ((int32_t)((uint32_t)(to) - (uint32_t)(from)))
#define TICKS_SINCE(t0)          TICKS_DISTANCE(t0, TICKS_READ())
#define TICKS_BEFORE(t1, t2)     (TICKS_DISTANCE(t1, t2) > 0)
#define TICKS_FROM_MS(val)       ((val) * (TICKS_PER_SECOND / 1000))
#define TICKS_TO_MS(val)         ((val) / (TICKS_PER_SECOND / 1000))
#define TICKS_TO_US(val)         ((val) * 8 / (8 * TICKS_PER_SECOND / 1000000))

static inline uint64_t get_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return (ns * TICKS_PER_SECOND) / 1000000000ull;
}
static inline uint64_t get_ticks_us(void) { return get_ticks() * 8 / (8 * TICKS_PER_SECOND / 1000000); }
static inline uint64_t get_ticks_ms(void) { return get_ticks() / (TICKS_PER_SECOND / 1000); }

static inline void wait_ms(unsigned long ms) {
    struct timespec ts = { .tv_sec = (time_t)(ms / 1000),
                           .tv_nsec = (long)((ms % 1000) * 1000000ul) };
    nanosleep(&ts, NULL);
}

/* ── surfaces and the display ──────────────────────────────────────────
 * Field names and order are libdragon's. The engine never reaches into a
 * surface_t — kiln_engine.c passes it straight back to rdpq_attach — but the
 * host rasteriser does, so it is a real struct rather than an opaque one. */
typedef struct surface_s {
    uint32_t flags;      /* format + ownership bits; the host uses RGBA32 only */
    uint16_t width;
    uint16_t height;
    uint16_t stride;     /* bytes per row */
    void    *buffer;
} surface_t;

typedef struct sprite_s sprite_t;

/* ── texture formats: the RDP's own encoding, copied ───────────────────
 * (fmt << 2) | size, so FMT_CI4 is 0x08 — which is exactly what the flags
 * byte of libdragon's builtin font atlas holds, and how tools/font_extract.py
 * identified it. Copied rather than renumbered for that reason: the value
 * appears in on-disk sprites. */
#define _RDP_FORMAT_CODE(fmt, size)   (((fmt) << 2) | (size))
#define TEX_FORMAT_BITDEPTH(fmt)      (4 << ((fmt) & 0x3))
#define TEX_FORMAT_PIX2BYTES(fmt, px) ((((px) << (((fmt) & 3) + 2)) + 7) >> 3)

typedef enum {
    FMT_NONE   = 0,
    FMT_RGBA16 = _RDP_FORMAT_CODE(0, 2),
    FMT_RGBA32 = _RDP_FORMAT_CODE(0, 3),
    FMT_YUV16  = _RDP_FORMAT_CODE(1, 2),
    FMT_CI4    = _RDP_FORMAT_CODE(2, 0),
    FMT_CI8    = _RDP_FORMAT_CODE(2, 1),
    FMT_IA4    = _RDP_FORMAT_CODE(3, 0),
    FMT_IA8    = _RDP_FORMAT_CODE(3, 1),
    FMT_IA16   = _RDP_FORMAT_CODE(3, 2),
    FMT_I4     = _RDP_FORMAT_CODE(4, 0),
    FMT_I8     = _RDP_FORMAT_CODE(4, 1),
} tex_format_t;

#define SURFACE_FLAGS_TEXFORMAT   0x1F
#define SURFACE_FLAGS_OWNEDBUFFER 0x20

static inline surface_t surface_make(void *buffer, tex_format_t format,
                                     uint16_t width, uint16_t height,
                                     uint16_t stride)
{
    return (surface_t){ .flags = (uint32_t)format, .width = width,
                        .height = height, .stride = stride, .buffer = buffer };
}

static inline surface_t surface_make_linear(void *buffer, tex_format_t format,
                                            uint16_t width, uint16_t height)
{
    return surface_make(buffer, format, width, height,
                        (uint16_t)TEX_FORMAT_PIX2BYTES(format, width));
}

static inline tex_format_t surface_get_format(const surface_t *s)
{
    return (tex_format_t)(s->flags & SURFACE_FLAGS_TEXFORMAT);
}

#define DEPTH_16_BPP 2
#define DEPTH_32_BPP 4
#define GAMMA_NONE   0
#define FILTERS_RESAMPLE 1
#define FILTERS_DISABLED 0
#define ANTIALIAS_RESAMPLE 1

void        display_init(resolution_t res, int bitdepth, uint32_t num_buffers,
                         int gamma, int filters);
void        display_close(void);
surface_t  *display_get(void);
surface_t  *display_get_zbuf(void);
int         display_get_width(void);
int         display_get_height(void);

/* ── rdpq: the subset the 2D pass uses ─────────────────────────────────
 * Combiner and blender values are libdragon's RDP register words on console
 * and host-local sentinels here — the engine only ever passes the named
 * macros, never a hand-assembled one, so the host does not need to emulate
 * the register encoding to route them correctly. If a caller ever builds a
 * combiner with RDPQ_COMBINER1() directly, this will need the real encoding.
 */
typedef uint64_t rdpq_combiner_t;
typedef uint32_t rdpq_blender_t;

#define RDPQ_COMBINER_FLAT      ((rdpq_combiner_t)1)  /* PRIM                 */
#define RDPQ_COMBINER_SHADE     ((rdpq_combiner_t)2)  /* vertex colour        */
#define RDPQ_COMBINER_TEX       ((rdpq_combiner_t)3)  /* texel               */
#define RDPQ_COMBINER_TEX_FLAT  ((rdpq_combiner_t)4)  /* texel * PRIM        */
#define RDPQ_COMBINER_TEX_SHADE ((rdpq_combiner_t)5)  /* texel * vertex      */
#define RDPQ_BLENDER_MULTIPLY   ((rdpq_blender_t)1)   /* src*a + dst*(1-a)   */

typedef enum { TILE0 = 0, TILE1, TILE2, TILE3,
               TILE4, TILE5, TILE6, TILE7 } rdpq_tile_t;

/* Vertex-array layout descriptor. pos_offset/shade_offset are indices into
 * the caller's float array, exactly as on console. */
typedef struct rdpq_trifmt_s {
    int  pos_offset;
    int  shade_offset;
    bool shade_flat;
    int  tex_offset;
    rdpq_tile_t tex_tile;
    int  tex_mipmaps;
    int  z_offset;
} rdpq_trifmt_t;

extern const rdpq_trifmt_t TRIFMT_FILL;
extern const rdpq_trifmt_t TRIFMT_SHADE;

void rdpq_init(void);
void rdpq_close(void);
void rdpq_attach(surface_t *color, surface_t *z);
void rdpq_attach_clear(surface_t *color, surface_t *z);
void rdpq_detach_show(void);
void rdpq_detach(void);
void rdpq_sync_pipe(void);
void rdpq_sync_tile(void);
void rdpq_set_mode_standard(void);
void rdpq_set_mode_fill(color_t c);
void rdpq_mode_combiner(rdpq_combiner_t comb);
void rdpq_mode_blender(rdpq_blender_t blend);
void rdpq_mode_zbuf(bool compare, bool write);
void rdpq_mode_alphacompare(int threshold);
void rdpq_mode_fog(rdpq_blender_t fog);
void rdpq_mode_antialias(int mode);
void rdpq_set_prim_color(color_t c);
void rdpq_set_fog_color(color_t c);
void rdpq_fill_rectangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void rdpq_triangle(const rdpq_trifmt_t *fmt, const float *v0,
                   const float *v1, const float *v2);
void rspq_wait(void);
void rspq_flush(void);

#define RDPQ_FOG_STANDARD ((rdpq_blender_t)2)

/* ── texturing ────────────────────────────────────────────────────────
 * The RDP samples from TMEM: 4 KB of on-chip memory that every texture must
 * be DMA'd into before it can be drawn. Overflowing it does not fail — it
 * wraps, and you get a texture built out of whatever else was resident. So
 * the host TRACKS occupancy and asserts, which is one of the few console
 * limits a host build can genuinely check. See kiln_host_tmem_used(). */
#define TMEM_BYTES 4096

#define REPEAT_INFINITE 2048
#define MIRROR_REPEAT   true
#define MIRROR_NONE     false

typedef struct rdpq_texparms_s {
    int tmem_addr;
    int palette;
    struct {
        float translate;
        int   scale_log;
        float repeats;
        bool  mirror;
    } s, t;
} rdpq_texparms_t;

typedef enum { TLUT_NONE = 0, TLUT_RGBA16 = 2, TLUT_IA16 = 3 } rdpq_tlut_t;

int  rdpq_tex_upload(rdpq_tile_t tile, const surface_t *tex,
                     const rdpq_texparms_t *parms);
void rdpq_tex_upload_tlut(uint16_t *tlut, int color_idx, int num_colors);
void rdpq_mode_tlut(rdpq_tlut_t tlut);
void rdpq_mode_persp(bool perspective);
void rdpq_mode_filter(int filter);
void rdpq_mode_dithering(int dither);
void rdpq_set_lookup_address(uint8_t index, void *rdram_addr);

#define FILTER_POINT     0
#define FILTER_BILINEAR  1
#define AA_STANDARD      1
#define AA_NONE          0
#define DITHER_NONE_NONE 0
#define DITHER_SQUARE_SQUARE 1

/** TMEM bytes currently occupied by uploaded textures and the TLUT. */
int kiln_host_tmem_used(void);

/* ── rdpq_font / rdpq_text ─────────────────────────────────────────────
 * The host draws with libdragon's OWN builtin font, decoded out of the same
 * blob the ROM links (plat/host/include/kiln_host_font.h, generated by
 * tools/font_extract.py). That is what makes a host HUD capture predict the
 * console's layout instead of merely resembling it. */
typedef struct rdpq_font_s rdpq_font_t;

typedef enum { FONT_BUILTIN_DEBUG_MONO = 1, FONT_BUILTIN_DEBUG_VAR = 2 } rdpq_font_builtin_t;

typedef struct { color_t color; color_t outline_color; } rdpq_fontstyle_t;

typedef struct {
    int16_t width, height;
    bool    wrap;
    uint8_t style_id;
    int16_t char_spacing;
    int16_t line_spacing;
} rdpq_textparms_t;

rdpq_font_t *rdpq_font_load_builtin(rdpq_font_builtin_t which);
rdpq_font_t *rdpq_font_load(const char *path);
void         rdpq_font_style(rdpq_font_t *font, uint8_t id,
                             const rdpq_fontstyle_t *style);
void         rdpq_text_register_font(uint8_t id, rdpq_font_t *font);
int          rdpq_text_print(const rdpq_textparms_t *parms, uint8_t font_id,
                             float x0, float y0, const char *utf8_text);

#define FONT_MAGIC_LOADED "FNL"

#endif /* KILN_HOST_LIBDRAGON_H */
