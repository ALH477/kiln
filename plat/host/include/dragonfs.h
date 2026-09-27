/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/dragonfs.h — the one DragonFS ioctl Kiln asks for.
 *
 * fig_sdbfs answers IODFS_GET_ROM_BASE so that libdragon's wav64 can take its
 * cartridge-DMA path: given a PI address it issues dma_read_async straight off
 * the cart, and given zero it falls back to lseek/read.
 *
 * On a host there is no cartridge, so the answer is always zero and the
 * fallback is not a fallback — it is the only path there is. That is exactly
 * the degradation kiln_sdbfs.h already documents, and it is why this header
 * needs to define the command and nothing else: the mechanism works here
 * unchanged, it simply always takes the other branch.
 */
#ifndef FIG_HOST_DRAGONFS_H
#define FIG_HOST_DRAGONFS_H

#include <sys/ioctl.h>

/* Verbatim from libdragon's dragonfs.h:79. */
#ifndef IODFS_GET_ROM_BASE
#define IODFS_GET_ROM_BASE _IO('D', 0)
#endif

#endif /* FIG_HOST_DRAGONFS_H */
