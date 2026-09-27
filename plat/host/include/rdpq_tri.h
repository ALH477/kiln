/* SPDX-License-Identifier: MIT
 *
 * plat/host/include/rdpq_tri.h — libdragon splits the triangle API into its
 * own header; the host keeps everything in one <libdragon.h>.
 *
 * This exists so a game that includes <rdpq_tri.h> — PetaByte Madness'
 * src/main.c and src/pm_veil.c both do — compiles unedited. rdpq_triangle and
 * rdpq_trifmt_t are already declared there; re-including is the whole file.
 */
#ifndef FIG_HOST_RDPQ_TRI_H
#define FIG_HOST_RDPQ_TRI_H
#include <libdragon.h>
#endif /* FIG_HOST_RDPQ_TRI_H */
