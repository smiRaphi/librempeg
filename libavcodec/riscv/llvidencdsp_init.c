/*
 * Copyright © 2023 Rémi Denis-Courmont.
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

#include "config.h"

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavcodec/lossless_videoencdsp.h"

void ff_llvidenc_diff_bytes_rvv(uint8_t *dst, const uint8_t *src1,
                                const uint8_t *src2, intptr_t w);

av_cold void ff_llvidencdsp_init_riscv(LLVidEncDSPContext *c)
{
#if HAVE_RVV
    int flags = av_get_cpu_flags();

    if (flags & AV_CPU_FLAG_RVV_I32) {
        c->diff_bytes = ff_llvidenc_diff_bytes_rvv;
    }
#endif
}
