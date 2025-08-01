/*
 * copyright (c) 2015 rcombs
 *
 * This file is part of Librempeg
 *
 * Librempeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Librempeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Librempeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef AVUTIL_AES_INTERNAL_H
#define AVUTIL_AES_INTERNAL_H

#include "mem_internal.h"
#include <stdint.h>

typedef union {
    uint64_t u64[2];
    uint32_t u32[4];
    uint8_t u8x4[4][4];
    uint8_t u8[16];
} av_aes_block;

typedef struct AVAES {
    // Note: round_key[16] is accessed in the init code, but this only
    // overwrites state, which does not matter (see also commit ba554c0).
    DECLARE_ALIGNED(16, av_aes_block, round_key)[15];
    DECLARE_ALIGNED(16, av_aes_block, state)[2];
    int rounds;
    void (*crypt)(struct AVAES *a, uint8_t *dst, const uint8_t *src, int count, uint8_t *iv, int rounds);
} AVAES;

void ff_init_aes_x86(AVAES *a, int decrypt);

#endif /* AVUTIL_AES_INTERNAL_H */
