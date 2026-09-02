/* SPDX-License-Identifier: MIT
 *
 * host_wav64.c — .wav64 becomes PCM.
 *
 * host_audio.c's header used to end "VADPCM decoding and an output device are
 * not here", and that was the right call while the host tier was only a gate:
 * a check exercises kiln_audio's channel arithmetic — the 32-channel budget,
 * the SFX/music partition, priority stealing, room crossfades — and none of
 * that needs a sample. A launcher does.
 *
 * ── Not a hand-written decoder ─────────────────────────────────────────
 * VADPCM is decoded on console by RSP microcode, which has no host analogue,
 * so the obvious move is to write a decoder from the format description. This
 * project has a rule against exactly that, and nix/host-math.nix states it at
 * length: a reimplementation that differs in the last bit makes the host
 * disagree with the console for reasons that have nothing to do with the code
 * under test.
 *
 * libdragon vendors Dietrich Epp's VADPCM codec (tools/audioconv64/vadpcm/,
 * MPL-2.0) and audioconv64 — the ENCODER that produced every .wav64 in this
 * repo — is built from it. So the host decodes with the decoder that belongs
 * to the encoder, compiled from the same pinned libdragon input, and the only
 * thing written here is the container: the 28-byte header, the codebook, and
 * the block-planar layout of a stereo file. See nix/host.nix's vadpcm
 * derivation and THIRD_PARTY_LICENSES.md.
 *
 * ── Everything in the file is big-endian ───────────────────────────────
 * Because the console is. Read byte-at-a-time through be16/be32 rather than
 * casting, for the reason host_t3dmodel.c's header gives: a cast is correct
 * on exactly the machines that already agree with the file, and the whole
 * point of this tier is the ones that do not.
 */
#include <libdragon.h>
#include "host_internal.h"

#include <codec/vadpcm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* wav64_internal.h's constants. Copied, not guessed: these four numbers are
 * the file's own vocabulary and appear in it. */
#define WAV64_FORMAT_RAW    0
#define WAV64_FORMAT_VADPCM 1
#define WAV64_FORMAT_ULC    2
#define WAV64_FORMAT_OPUS   3
#define VADPCM_FLAG_HUFFMAN (1 << 0)

/* Stereo VADPCM is stored as blocks of this many frames per channel, L then
 * R. Mono is one linear plane. */
#define VADPCM_BLOCK_FRAMES 128

/* ── The Huffman layer ─────────────────────────────────────────────────
 * audioconv64 Huffman-codes the VADPCM nibbles by default, so every .wav64 in
 * this repo carries this and the plain path alone decodes none of them.
 *
 * On console this layer is decompressed by the CPU — libdragon's
 * huffv_decompress in src/audio/wav64_vadpcm.c — and only the ADPCM below it
 * runs on the RSP. So unlike the ADPCM, a C implementation of it does exist;
 * it is `static` inside a translation unit that pulls in rspq, the mixer's
 * internals and the samplebuffer, none of which can compile natively, so it
 * cannot be reused the way the codec in tools/audioconv64/vadpcm/ is.
 *
 * What follows is therefore a transcription of that routine and of
 * wav64_vadpcm_init_huffman, and the transcription is the risk: this is
 * exactly the "second implementation" this project argues against everywhere
 * else. Two things hold it: the bitstream is byte-exact or it produces noise
 * rather than something subtly off, and nix/checks/kiln-wav64.nix decodes a
 * real audioconv64 file and measures the RMS of the result — a wrong Huffman
 * table does not produce quiet audio, it produces garbage or an assert.
 *
 * If libdragon ever exposes this as a standalone function, delete this and
 * call it. */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static const char *fmt_name(int f)
{
    switch (f) {
    case WAV64_FORMAT_RAW:    return "raw";
    case WAV64_FORMAT_VADPCM: return "vadpcm";
    case WAV64_FORMAT_ULC:    return "ulc";
    case WAV64_FORMAT_OPUS:   return "opus";
    default:                  return "unknown";
    }
}

/* Whole file into memory. Sound effects are kilobytes and the host is not a
 * 4 MB console; streaming here would buy nothing and cost a second code path
 * that only the long files would ever exercise. */
static uint8_t *slurp(const char *path, int *out_size)
{
    /* <= 0, not < 0: dfs_open reserves handle 0 and returns DFS_ENOFILE for a
     * miss, so a `< 0` test would let 0 through and hand it to dfs_size — the
     * same off-by-one kiln_cache's handle packing already cost this project
     * once. */
    const int fd = dfs_open(path);
    if (fd <= 0) return NULL;
    const int sz = dfs_size(fd);
    if (sz <= 0) { dfs_close(fd); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { dfs_close(fd); return NULL; }
    const int got = dfs_read(buf, 1, sz, fd);
    dfs_close(fd);
    if (got != sz) { free(buf); return NULL; }
    *out_size = sz;
    return buf;
}

/* One 256-entry lookup per context: index by the next 8 bits, get back
 * (symbol << 4) | length. Three contexts — the two nibbles of a frame's
 * header byte use tables 0 and 1, every other nibble uses table 2. */
typedef struct { uint8_t codes[256]; } HuffTable;

static int huff_build(const uint8_t *ctx, HuffTable tbl[3], char *err, size_t errn)
{
    memset(tbl, 0, sizeof(HuffTable) * 3);
    for (int i = 0; i < 3; i++) {
        /* wav64_vadpcm_huffctx_t: uint8 lengths[8] (two 4-bit lengths per
         * byte, high nibble first), then uint8 values[16]. All bytes, so no
         * byte order to get wrong. */
        const uint8_t *lengths = ctx + (size_t)i * 24;
        const uint8_t *values  = lengths + 8;
        for (int j = 0; j < 16; j++) {
            const int len = (lengths[j / 2] >> (4 * (~j & 1))) & 0xF;
            if (len == 0xF) continue;              /* symbol unused */
            if (len > 8 || (values[j] >> len) != 0) {
                snprintf(err, errn, "bad huffman symbol %d in context %d", j, i);
                return -1;
            }
            const int shift = 8 - len;
            const int code = values[j] << shift;
            const uint8_t v = (uint8_t)((j << 4) | len);
            for (int k = 0; k < (1 << shift); k++) {
                /* Upstream asserts this slot is still free. Kept, because a
                 * table whose symbols OVERLAP can still cover all 256
                 * prefixes — so the coverage check below would pass it, and
                 * it would decode to plausible-looking wrong audio rather
                 * than to an error. */
                if (tbl[i].codes[code + k] != 0) {
                    snprintf(err, errn,
                             "huffman context %d: symbol %d overlaps prefix 0x%02x",
                             i, j, code + k);
                    return -1;
                }
                tbl[i].codes[code + k] = v;
            }
        }
        for (int j = 0; j < 256; j++) {
            if (tbl[i].codes[j] == 0) {
                snprintf(err, errn, "huffman context %d does not cover prefix 0x%02x", i, j);
                return -1;
            }
        }
    }
    return 0;
}

/* Expand the whole stream to plain 9-byte frames. `dst` holds nframes*9
 * bytes. Reads 32 bits at a time, big-endian, exactly as the console does —
 * there the load is native and here it is assembled, which is the only
 * difference. */
static int huff_expand(const uint8_t *src, size_t avail, uint8_t *dst, size_t len,
                       const HuffTable tbl[3], char *err, size_t errn)
{
    uint64_t buffer = 0;
    int      bits   = 0;
    size_t   at     = 0;

    /* Upstream opens with a `if (bitpos & 7)` partial-byte prologue, because
     * on console this runs per samplebuffer fill and resumes mid-stream at a
     * saved bit position. Here the whole body is expanded once from bit zero,
     * so there is no partial byte to resume from and the prologue has nothing
     * to do. That also means none of upstream's skip-point machinery is
     * needed: seeking is a streaming concern, and nothing streams here. */

    for (size_t i = 0; i < len; i += 9) {
        int t = 0;
        for (int j = 0; j < 9; j++) {
            while (bits < 32) {
                /* The last refill of a stream always overruns: the decoder
                 * pulls 32 bits at a time and the final frame needs only a
                 * few of them. On console the read lands in a scratch buffer
                 * with slack after it; here the tail is zero-padded, which is
                 * the same thing without the buffer. Only a refill that is
                 * ENTIRELY past the end is a real truncation. */
                if (at >= avail + 4) {
                    snprintf(err, errn, "huffman stream ran out after %zu of %zu bytes",
                             i, len);
                    return -1;
                }
                uint32_t w = 0;
                for (int b = 0; b < 4; b++) {
                    const size_t k = at + (size_t)b;
                    w = (w << 8) | (k < avail ? src[k] : 0u);
                }
                buffer = (buffer << 32) | (uint64_t)w;
                at += 4;
                bits += 32;
            }
            const uint8_t c1 = tbl[t].codes[(buffer >> (bits - 8)) & 0xFF];
            bits -= c1 & 0xF;
            if (j == 0) t++;
            const uint8_t c2 = tbl[t].codes[(buffer >> (bits - 8)) & 0xFF];
            bits -= c2 & 0xF;
            if (j == 0) t++;
            dst[i + (size_t)j] = (uint8_t)(((c1 >> 4) << 4) | (c2 >> 4));
        }
    }
    return 0;
}

/* Where channel `ch`'s frame `frame` lives in the file. Mono is linear;
 * stereo interleaves 128-frame blocks, and the LAST block is short — which is
 * the detail worth copying rather than deriving, because getting it wrong
 * only corrupts the final fraction of a second of a sound. */
static int vadpcm_file_index(int frame, int ch, int nframes, int channels)
{
    if (channels == 1) return frame;
    const int B = VADPCM_BLOCK_FRAMES;
    const int block = frame / B;
    const int off   = frame % B;
    const int nblocks = (nframes + B - 1) / B;
    const int bs = (block == nblocks - 1) ? (nframes - block * B) : B;
    return block * B * channels + ch * bs + off;
}

static int decode_vadpcm(const uint8_t *f, int size, int nch, int nsamples,
                         int16_t *pcm, char *err, size_t errn)
{
    const uint8_t *ext = f + 28;
    if (size < 28 + 96) { snprintf(err, errn, "truncated vadpcm header"); return -1; }

    const int npred = (int8_t)ext[0];
    const int order = (int8_t)ext[1];
    const uint16_t flags = be16(ext + 2);
    if (npred <= 0 || order <= 0 || order > 8) {
        snprintf(err, errn, "vadpcm codebook is %dx%d", npred, order);
        return -1;
    }

    /* The codebook follows the 96-byte extended header. Its int16 vectors are
     * big-endian like everything else in the file. */
    const int nvec = npred * order;
    struct vadpcm_vector *cb = calloc((size_t)nvec, sizeof *cb);
    if (!cb) { snprintf(err, errn, "out of memory"); return -1; }
    const uint8_t *cbsrc = ext + 96;
    if (28 + 96 + nvec * 16 > size) {
        free(cb); snprintf(err, errn, "truncated vadpcm codebook"); return -1;
    }
    for (int v = 0; v < nvec; v++)
        for (int i = 0; i < 8; i++)
            cb[v].v[i] = (int16_t)be16(cbsrc + (size_t)v * 16 + (size_t)i * 2);

    const uint32_t start = be32(f + 20);   /* start_offset */
    if (start >= (uint32_t)size) {
        free(cb); snprintf(err, errn, "vadpcm data starts past end of file"); return -1;
    }

    /* audioconv64 pads to a multiple of 32 samples before encoding but writes
     * the unpadded length, so the file holds more frames than the waveform
     * addresses. Decode what the file has and keep what the header claims. */
    const int frames_per_ch = ((nsamples + 31) / 32) * 2;
    const size_t need = (size_t)frames_per_ch * nch * kVADPCMFrameByteSize;

    /* Huffman first, if present: it wraps the whole body, so it has to be
     * expanded before any frame can be addressed by index. */
    const uint8_t *frames_src = f + start;
    uint8_t *expanded = NULL;
    const int total_frames = frames_per_ch * nch;
    if (flags & VADPCM_FLAG_HUFFMAN) {
        HuffTable tbl[3];
        if (huff_build(ext + 20, tbl, err, errn)) { free(cb); return -1; }
        expanded = malloc((size_t)total_frames * kVADPCMFrameByteSize);
        if (!expanded) { free(cb); snprintf(err, errn, "out of memory"); return -1; }
        if (huff_expand(f + start, (size_t)size - start, expanded,
                        (size_t)total_frames * kVADPCMFrameByteSize, tbl, err, errn)) {
            free(expanded); free(cb); return -1;
        }
        frames_src = expanded;
    } else if (start + need > (uint32_t)size) {
        free(cb);
        snprintf(err, errn, "vadpcm body is %zu bytes short",
                 (size_t)(start + need - (uint32_t)size));
        return -1;
    }

    int16_t *plane = malloc((size_t)kVADPCMFrameSampleCount * sizeof(int16_t));
    if (!plane) {
        free(expanded); free(cb);
        snprintf(err, errn, "out of memory");
        return -1;
    }

    int rc = 0;
    for (int c = 0; c < nch && rc == 0; c++) {
        struct vadpcm_vector state;
        memset(&state, 0, sizeof state);

        for (int fr = 0; fr < frames_per_ch; fr++) {
            const int fi = vadpcm_file_index(fr, c, frames_per_ch, nch);
            const vadpcm_error e = vadpcm_decode(
                npred, order, cb, &state, 1, plane,
                frames_src + (size_t)fi * kVADPCMFrameByteSize);
            if (e != kVADPCMErrNone) {
                snprintf(err, errn, "vadpcm_decode: %s", vadpcm_error_name(e));
                rc = -1;
                break;
            }
            /* Interleave straight into the output. The console's mixer takes
             * planes; this one takes frames, because a host mixer walking two
             * planes would be a second layout to keep straight for no gain. */
            for (int i = 0; i < kVADPCMFrameSampleCount; i++) {
                const int smp = fr * kVADPCMFrameSampleCount + i;
                if (smp >= nsamples) break;
                pcm[(size_t)smp * nch + c] = plane[i];
            }
        }
    }

    free(expanded);
    free(plane);
    free(cb);
    return rc;
}

static int decode_raw(const uint8_t *f, int size, int nch, int nbits,
                      int nsamples, int16_t *pcm, char *err, size_t errn)
{
    const uint32_t start = be32(f + 20);
    const size_t need = (size_t)nsamples * nch * (nbits == 8 ? 1 : 2);
    if (start + need > (uint32_t)size) {
        snprintf(err, errn, "raw body is %zu bytes short",
                 (size_t)(start + need - (uint32_t)size));
        return -1;
    }
    const uint8_t *s = f + start;
    const size_t n = (size_t)nsamples * nch;
    if (nbits == 8) for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)((int8_t)s[i] << 8);
    else            for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)be16(s + i * 2);
    return 0;
}

KilnHostWave *kiln_host_wave_load(const char *path, char *err, size_t errn)
{
    err[0] = '\0';

    int size = 0;
    uint8_t *f = slurp(path, &size);
    if (!f) { snprintf(err, errn, "not found or unreadable"); return NULL; }
    if (size < 28 || memcmp(f, "WV64", 4) != 0) {
        free(f); snprintf(err, errn, "not a .wav64 (bad magic)"); return NULL;
    }

    const int format   = (int8_t)f[5];
    const int channels = (int8_t)f[6];
    const int nbits    = (int8_t)f[7];
    const int freq     = (int32_t)be32(f + 8);
    const int len      = (int32_t)be32(f + 12);
    const int loop_len = (int32_t)be32(f + 16);

    if (channels < 1 || channels > 2 || len <= 0) {
        free(f);
        snprintf(err, errn, "%d channels, %d samples", channels, len);
        return NULL;
    }

    KilnHostWave *w = calloc(1, sizeof *w);
    int16_t *pcm = calloc((size_t)len * channels, sizeof(int16_t));
    if (!w || !pcm) {
        free(w); free(pcm); free(f);
        snprintf(err, errn, "out of memory");
        return NULL;
    }

    int rc;
    switch (format) {
    case WAV64_FORMAT_RAW:
        rc = decode_raw(f, size, channels, nbits, len, pcm, err, errn); break;
    case WAV64_FORMAT_VADPCM:
        rc = decode_vadpcm(f, size, channels, len, pcm, err, errn); break;
    default:
        /* ULC and Opus both decode on the RSP with no C fallback in
         * libdragon's tree, so there is nothing to compile natively and
         * nothing honest to write here. */
        snprintf(err, errn, "%s is not decoded on the host", fmt_name(format));
        rc = -1;
        break;
    }
    free(f);
    if (rc) { free(w); free(pcm); return NULL; }

    w->pcm      = pcm;
    w->samples  = len;
    w->channels = channels;
    w->rate     = freq > 0 ? freq : 32000;
    w->loop_len = loop_len;
    return w;
}

void kiln_host_wave_free(KilnHostWave *w)
{
    if (!w) return;
    free(w->pcm);
    free(w);
}
