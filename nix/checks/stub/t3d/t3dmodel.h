/* SPDX-License-Identifier: MIT
 *
 * Stub t3d/t3dmodel.h for the kiln-asset host check. Defines the minimum
 * surface kiln_asset.c references (T3DModel, t3d_model_load_buf). The stub
 * load_buf records the buffer; it does not parse, since libt3d.a is
 * MIPS-only.
 */
#ifndef STUB_T3DMODEL_H
#define STUB_T3DMODEL_H

#include <stdint.h>

typedef struct T3DModel T3DModel;
struct T3DModel { uint8_t _opaque; };

extern void *g_last_model_buf;
extern int   g_last_model_sz;

/* Returns `buf` as a T3DModel* so kiln_asset_model's "free via
 * t3d_model_free" contract holds in the host test. A real t3d_model_load_buf
 * would parse the buffer; the ROM demo on Ares covers that path. */
static inline T3DModel *t3d_model_load_buf(void *buf, int sz) {
    g_last_model_buf = buf;
    g_last_model_sz  = sz;
    return (T3DModel *)buf;
}

static inline void t3d_model_free(T3DModel *m) { free(m); }

#endif /* STUB_T3DMODEL_H */