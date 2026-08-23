/* SPDX-License-Identifier: MIT
 *
 * host_png.c — write the host framebuffer as a PNG.
 *
 * PNG rather than PPM because these files get committed as reference images
 * and looked at by people: a 320x240 PPM is 230 KB of uncompressed bytes per
 * frame, a PNG of the same flat-shaded HUD is a couple of KB.
 *
 * Determinism is the property that matters, since a reference image is only
 * useful if the same framebuffer always produces the same file. Filter 0
 * (None) on every row and a fixed zlib level give that; no adaptive filtering,
 * no timestamp chunk, no encoder heuristics that could change with a zlib
 * bump. CLAUDE.md's note that two `./dev shot` runs of the same deterministic
 * ROM differ in ~37 pixels of compositor chrome is the console-side version of
 * this problem — here there is no compositor, so the bytes are simply equal.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *tag, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    uint32_t c = (uint32_t)crc32(crc32(0L, Z_NULL, 0), (const Bytef *)tag, 4);
    if (len) c = (uint32_t)crc32(c, (const Bytef *)data, len);
    uint8_t crcb[4];
    be32(crcb, c);
    return fwrite(crcb, 1, 4, f) == 4 ? 0 : -1;
}

int kiln_host_write_png(const char *path, const uint8_t *rgba, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;

    /* One filter byte per row, then the row's RGBA. */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * (1 + (size_t)w * 4);
        *dst++ = 0;                                   /* filter: None */
        memcpy(dst, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }

    uLongf zlen = compressBound((uLong)raw_len);
    uint8_t *z = malloc(zlen);
    if (!z) { free(raw); return -1; }
    if (compress2(z, &zlen, raw, (uLong)raw_len, 6) != Z_OK) {
        free(raw); free(z); return -1;
    }
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    int rc = (fwrite(sig, 1, 8, f) == 8) ? 0 : -1;

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;   /* bit depth   */
    ihdr[9]  = 6;   /* colour type: RGBA */
    ihdr[10] = 0;   /* deflate     */
    ihdr[11] = 0;   /* filter      */
    ihdr[12] = 0;   /* no interlace */
    if (!rc) rc = chunk(f, "IHDR", ihdr, sizeof ihdr);
    if (!rc) rc = chunk(f, "IDAT", z, (uint32_t)zlen);
    if (!rc) rc = chunk(f, "IEND", NULL, 0);

    free(z);
    if (fclose(f) != 0) rc = -1;
    return rc;
}
