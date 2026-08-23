/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/libcart/cart.h — the host's <libcart/cart.h>.
 *
 * There is no flashcart. cart_init() reports failure and cart_type stays
 * CART_NULL, which is the honest answer and also the one kiln_store is already
 * written to handle: it walks SD -> save chip -> read-only ROM and reports
 * which backend it got. On the host it gets none of the three and falls
 * through to KILN_STORE_DFS over the host VFS.
 *
 * Reporting "no cart" rather than pretending is load-bearing. kiln_store's own
 * history is the argument: sram_detect() returning 0 instead of -1 meant the
 * SRAM backend was selected on machines with no chip at all, after which
 * writes went nowhere and reads came back as zeros — which parse as a valid
 * EMPTY directory. A backend that claims to work and does not is worse than
 * one that says no.
 */
#ifndef KILN_HOST_LIBCART_H
#define KILN_HOST_LIBCART_H

#include <stdint.h>

#define CART_NULL (-1)
#define CART_CI    0   /* 64drive                                          */
#define CART_EDX   1   /* EverDrive-64 X-series                            */
#define CART_ED    2   /* EverDrive-64 V1/V2/V2.5/V3 and ED64+             */
#define CART_SC    3   /* SummerCart64                                     */

extern int cart_type;

int cart_init(void);
int cart_exit(void);
int cart_card_init(void);
int cart_card_rd_dram(void *dram, uint32_t lba, uint32_t count);
int cart_card_wr_dram(const void *dram, uint32_t lba, uint32_t count);

#endif /* KILN_HOST_LIBCART_H */
