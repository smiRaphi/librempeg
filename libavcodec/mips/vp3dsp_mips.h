/*
 * Copyright (c) 2018 gxw <guxiwei-hf@loongson.cn>
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

#ifndef AVCODEC_MIPS_VP3DSP_MIPS_H
#define AVCODEC_MIPS_VP3DSP_MIPS_H

#include "libavcodec/vp3dsp.h"
#include <string.h>

void ff_vp3_idct_add_msa(uint8_t *dest, ptrdiff_t line_size, int16_t *block);
void ff_vp3_idct_put_msa(uint8_t *dest, ptrdiff_t line_size, int16_t *block);
void ff_vp3_idct_dc_add_msa(uint8_t *dest, ptrdiff_t line_size, int16_t *block);
void ff_vp3_v_loop_filter_msa(uint8_t *first_pixel, ptrdiff_t stride,
                              int *bounding_values);
void ff_put_no_rnd_pixels_l2_msa(uint8_t *dst, const uint8_t *src1,
                                 const uint8_t *src2, ptrdiff_t stride, int h);
void ff_vp3_h_loop_filter_msa(uint8_t *first_pixel, ptrdiff_t stride,
                              int *bounding_values);

void ff_vp3_idct_add_mmi(uint8_t *dest, ptrdiff_t line_size, int16_t *block);
void ff_vp3_idct_put_mmi(uint8_t *dest, ptrdiff_t line_size, int16_t *block);
void ff_vp3_idct_dc_add_mmi(uint8_t *dest, ptrdiff_t line_size, int16_t *block);
void ff_put_no_rnd_pixels_l2_mmi(uint8_t *dst, const uint8_t *src1,
                                 const uint8_t *src2, ptrdiff_t stride, int h);

#endif /* #ifndef AVCODEC_MIPS_VP3DSP_MIPS_H */
