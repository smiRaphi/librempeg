/*
 * VP9 compatible video decoder
 *
 * Copyright (C) 2013 Ronald S. Bultje <rsbultje gmail com>
 * Copyright (C) 2013 Clément Bœsch <u pkh me>
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

#include "libavutil/common.h"
#include "bit_depth_template.c"
#include "vp9dsp.h"

#if BIT_DEPTH != 12

// FIXME see whether we can merge parts of this (perhaps at least 4x4 and 8x8)
// back with h264pred.[ch]

static void vert_4x4_c(uint8_t *restrict _dst, ptrdiff_t stride,
                       const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    pixel4 p4 = AV_RN4PA(top);

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, p4);
    AV_WN4PA(dst + stride * 1, p4);
    AV_WN4PA(dst + stride * 2, p4);
    AV_WN4PA(dst + stride * 3, p4);
}

static void vert_8x8_c(uint8_t *restrict _dst, ptrdiff_t stride,
                       const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
#if BIT_DEPTH == 8
    uint64_t p8 = AV_RN64A(top);
#else
    pixel4 p4a = AV_RN4PA(top + 0);
    pixel4 p4b = AV_RN4PA(top + 4);
#endif
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
#if BIT_DEPTH == 8
        AV_WN64A(dst, p8);
#else
        AV_WN4PA(dst + 0, p4a);
        AV_WN4PA(dst + 4, p4b);
#endif
        dst += stride;
    }
}

static void vert_16x16_c(uint8_t *restrict _dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
#if BIT_DEPTH == 8
    uint64_t p8a = AV_RN64A(top);
    uint64_t p8b = AV_RN64A(top + 8);
#else
    pixel4 p4a = AV_RN4PA(top +  0);
    pixel4 p4b = AV_RN4PA(top +  4);
    pixel4 p4c = AV_RN4PA(top +  8);
    pixel4 p4d = AV_RN4PA(top + 12);
#endif
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
#if BIT_DEPTH == 8
        AV_WN64A(dst +  0, p8a);
        AV_WN64A(dst +  8, p8b);
#else
        AV_WN4PA(dst +  0, p4a);
        AV_WN4PA(dst +  4, p4b);
        AV_WN4PA(dst +  8, p4c);
        AV_WN4PA(dst + 12, p4d);
#endif
        dst += stride;
    }
}

static void vert_32x32_c(uint8_t *restrict _dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
#if BIT_DEPTH == 8
    uint64_t p8a = AV_RN64A(top);
    uint64_t p8b = AV_RN64A(top + 8);
    uint64_t p8c = AV_RN64A(top + 16);
    uint64_t p8d = AV_RN64A(top + 24);
#else
    pixel4 p4a = AV_RN4PA(top +  0);
    pixel4 p4b = AV_RN4PA(top +  4);
    pixel4 p4c = AV_RN4PA(top +  8);
    pixel4 p4d = AV_RN4PA(top + 12);
    pixel4 p4e = AV_RN4PA(top + 16);
    pixel4 p4f = AV_RN4PA(top + 20);
    pixel4 p4g = AV_RN4PA(top + 24);
    pixel4 p4h = AV_RN4PA(top + 28);
#endif
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
#if BIT_DEPTH == 8
        AV_WN64A(dst +  0, p8a);
        AV_WN64A(dst +  8, p8b);
        AV_WN64A(dst + 16, p8c);
        AV_WN64A(dst + 24, p8d);
#else
        AV_WN4PA(dst +  0, p4a);
        AV_WN4PA(dst +  4, p4b);
        AV_WN4PA(dst +  8, p4c);
        AV_WN4PA(dst + 12, p4d);
        AV_WN4PA(dst + 16, p4e);
        AV_WN4PA(dst + 20, p4f);
        AV_WN4PA(dst + 24, p4g);
        AV_WN4PA(dst + 28, p4h);
#endif
        dst += stride;
    }
}

static void hor_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                      const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, PIXEL_SPLAT_X4(left[3]));
    AV_WN4PA(dst + stride * 1, PIXEL_SPLAT_X4(left[2]));
    AV_WN4PA(dst + stride * 2, PIXEL_SPLAT_X4(left[1]));
    AV_WN4PA(dst + stride * 3, PIXEL_SPLAT_X4(left[0]));
}

static void hor_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                      const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        pixel4 p4 = PIXEL_SPLAT_X4(left[7 - y]);

        AV_WN4PA(dst + 0, p4);
        AV_WN4PA(dst + 4, p4);
        dst += stride;
    }
}

static void hor_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                        const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        pixel4 p4 = PIXEL_SPLAT_X4(left[15 - y]);

        AV_WN4PA(dst +  0, p4);
        AV_WN4PA(dst +  4, p4);
        AV_WN4PA(dst +  8, p4);
        AV_WN4PA(dst + 12, p4);
        dst += stride;
    }
}

static void hor_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                        const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        pixel4 p4 = PIXEL_SPLAT_X4(left[31 - y]);

        AV_WN4PA(dst +  0, p4);
        AV_WN4PA(dst +  4, p4);
        AV_WN4PA(dst +  8, p4);
        AV_WN4PA(dst + 12, p4);
        AV_WN4PA(dst + 16, p4);
        AV_WN4PA(dst + 20, p4);
        AV_WN4PA(dst + 24, p4);
        AV_WN4PA(dst + 28, p4);
        dst += stride;
    }
}

#endif /* BIT_DEPTH != 12 */

static void tm_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                     const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    int y, tl = top[-1];

    stride /= sizeof(pixel);
    for (y = 0; y < 4; y++) {
        int l_m_tl = left[3 - y] - tl;

        dst[0] = av_clip_pixel(top[0] + l_m_tl);
        dst[1] = av_clip_pixel(top[1] + l_m_tl);
        dst[2] = av_clip_pixel(top[2] + l_m_tl);
        dst[3] = av_clip_pixel(top[3] + l_m_tl);
        dst += stride;
    }
}

static void tm_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                     const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    int y, tl = top[-1];

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        int l_m_tl = left[7 - y] - tl;

        dst[0] = av_clip_pixel(top[0] + l_m_tl);
        dst[1] = av_clip_pixel(top[1] + l_m_tl);
        dst[2] = av_clip_pixel(top[2] + l_m_tl);
        dst[3] = av_clip_pixel(top[3] + l_m_tl);
        dst[4] = av_clip_pixel(top[4] + l_m_tl);
        dst[5] = av_clip_pixel(top[5] + l_m_tl);
        dst[6] = av_clip_pixel(top[6] + l_m_tl);
        dst[7] = av_clip_pixel(top[7] + l_m_tl);
        dst += stride;
    }
}

static void tm_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                       const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    int y, tl = top[-1];

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        int l_m_tl = left[15 - y] - tl;

        dst[ 0] = av_clip_pixel(top[ 0] + l_m_tl);
        dst[ 1] = av_clip_pixel(top[ 1] + l_m_tl);
        dst[ 2] = av_clip_pixel(top[ 2] + l_m_tl);
        dst[ 3] = av_clip_pixel(top[ 3] + l_m_tl);
        dst[ 4] = av_clip_pixel(top[ 4] + l_m_tl);
        dst[ 5] = av_clip_pixel(top[ 5] + l_m_tl);
        dst[ 6] = av_clip_pixel(top[ 6] + l_m_tl);
        dst[ 7] = av_clip_pixel(top[ 7] + l_m_tl);
        dst[ 8] = av_clip_pixel(top[ 8] + l_m_tl);
        dst[ 9] = av_clip_pixel(top[ 9] + l_m_tl);
        dst[10] = av_clip_pixel(top[10] + l_m_tl);
        dst[11] = av_clip_pixel(top[11] + l_m_tl);
        dst[12] = av_clip_pixel(top[12] + l_m_tl);
        dst[13] = av_clip_pixel(top[13] + l_m_tl);
        dst[14] = av_clip_pixel(top[14] + l_m_tl);
        dst[15] = av_clip_pixel(top[15] + l_m_tl);
        dst += stride;
    }
}

static void tm_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                       const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    int y, tl = top[-1];

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        int l_m_tl = left[31 - y] - tl;

        dst[ 0] = av_clip_pixel(top[ 0] + l_m_tl);
        dst[ 1] = av_clip_pixel(top[ 1] + l_m_tl);
        dst[ 2] = av_clip_pixel(top[ 2] + l_m_tl);
        dst[ 3] = av_clip_pixel(top[ 3] + l_m_tl);
        dst[ 4] = av_clip_pixel(top[ 4] + l_m_tl);
        dst[ 5] = av_clip_pixel(top[ 5] + l_m_tl);
        dst[ 6] = av_clip_pixel(top[ 6] + l_m_tl);
        dst[ 7] = av_clip_pixel(top[ 7] + l_m_tl);
        dst[ 8] = av_clip_pixel(top[ 8] + l_m_tl);
        dst[ 9] = av_clip_pixel(top[ 9] + l_m_tl);
        dst[10] = av_clip_pixel(top[10] + l_m_tl);
        dst[11] = av_clip_pixel(top[11] + l_m_tl);
        dst[12] = av_clip_pixel(top[12] + l_m_tl);
        dst[13] = av_clip_pixel(top[13] + l_m_tl);
        dst[14] = av_clip_pixel(top[14] + l_m_tl);
        dst[15] = av_clip_pixel(top[15] + l_m_tl);
        dst[16] = av_clip_pixel(top[16] + l_m_tl);
        dst[17] = av_clip_pixel(top[17] + l_m_tl);
        dst[18] = av_clip_pixel(top[18] + l_m_tl);
        dst[19] = av_clip_pixel(top[19] + l_m_tl);
        dst[20] = av_clip_pixel(top[20] + l_m_tl);
        dst[21] = av_clip_pixel(top[21] + l_m_tl);
        dst[22] = av_clip_pixel(top[22] + l_m_tl);
        dst[23] = av_clip_pixel(top[23] + l_m_tl);
        dst[24] = av_clip_pixel(top[24] + l_m_tl);
        dst[25] = av_clip_pixel(top[25] + l_m_tl);
        dst[26] = av_clip_pixel(top[26] + l_m_tl);
        dst[27] = av_clip_pixel(top[27] + l_m_tl);
        dst[28] = av_clip_pixel(top[28] + l_m_tl);
        dst[29] = av_clip_pixel(top[29] + l_m_tl);
        dst[30] = av_clip_pixel(top[30] + l_m_tl);
        dst[31] = av_clip_pixel(top[31] + l_m_tl);
        dst += stride;
    }
}

#if BIT_DEPTH != 12

static void dc_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                     const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4((left[0] + left[1] + left[2] + left[3] +
                                top[0] + top[1] + top[2] + top[3] + 4) >> 3);

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, dc);
    AV_WN4PA(dst + stride * 1, dc);
    AV_WN4PA(dst + stride * 2, dc);
    AV_WN4PA(dst + stride * 3, dc);
}

static void dc_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                     const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4
        ((left[0] + left[1] + left[2] + left[3] + left[4] + left[5] +
          left[6] + left[7] + top[0] + top[1] + top[2] + top[3] +
          top[4] + top[5] + top[6] + top[7] + 8) >> 4);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        AV_WN4PA(dst + 0, dc);
        AV_WN4PA(dst + 4, dc);
        dst += stride;
    }
}

static void dc_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                       const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4
        ((left[0] + left[1] + left[2] + left[3] + left[4] + left[5] + left[6] +
          left[7] + left[8] + left[9] + left[10] + left[11] + left[12] +
          left[13] + left[14] + left[15] + top[0] + top[1] + top[2] + top[3] +
          top[4] + top[5] + top[6] + top[7] + top[8] + top[9] + top[10] +
          top[11] + top[12] + top[13] + top[14] + top[15] + 16) >> 5);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        AV_WN4PA(dst +  0, dc);
        AV_WN4PA(dst +  4, dc);
        AV_WN4PA(dst +  8, dc);
        AV_WN4PA(dst + 12, dc);
        dst += stride;
    }
}

static void dc_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                       const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4
        ((left[0] + left[1] + left[2] + left[3] + left[4] + left[5] + left[6] +
          left[7] + left[8] + left[9] + left[10] + left[11] + left[12] +
          left[13] + left[14] + left[15] + left[16] + left[17] + left[18] +
          left[19] + left[20] + left[21] + left[22] + left[23] + left[24] +
          left[25] + left[26] + left[27] + left[28] + left[29] + left[30] +
          left[31] + top[0] + top[1] + top[2] + top[3] + top[4] + top[5] +
          top[6] + top[7] + top[8] + top[9] + top[10] + top[11] + top[12] +
          top[13] + top[14] + top[15] + top[16] + top[17] + top[18] + top[19] +
          top[20] + top[21] + top[22] + top[23] + top[24] + top[25] + top[26] +
          top[27] + top[28] + top[29] + top[30] + top[31] + 32) >> 6);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        AV_WN4PA(dst +  0, dc);
        AV_WN4PA(dst +  4, dc);
        AV_WN4PA(dst +  8, dc);
        AV_WN4PA(dst + 12, dc);
        AV_WN4PA(dst + 16, dc);
        AV_WN4PA(dst + 20, dc);
        AV_WN4PA(dst + 24, dc);
        AV_WN4PA(dst + 28, dc);
        dst += stride;
    }
}

static void dc_left_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                          const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    pixel4 dc = PIXEL_SPLAT_X4((left[0] + left[1] + left[2] + left[3] + 2) >> 2);

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, dc);
    AV_WN4PA(dst + stride * 1, dc);
    AV_WN4PA(dst + stride * 2, dc);
    AV_WN4PA(dst + stride * 3, dc);
}

static void dc_left_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                          const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    pixel4 dc = PIXEL_SPLAT_X4
        ((left[0] + left[1] + left[2] + left[3] +
          left[4] + left[5] + left[6] + left[7] + 4) >> 3);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        AV_WN4PA(dst + 0, dc);
        AV_WN4PA(dst + 4, dc);
        dst += stride;
    }
}

static void dc_left_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                            const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    pixel4 dc = PIXEL_SPLAT_X4
        ((left[0] + left[1] + left[2] + left[3] + left[4] + left[5] +
          left[6] + left[7] + left[8] + left[9] + left[10] + left[11] +
          left[12] + left[13] + left[14] + left[15] + 8) >> 4);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        AV_WN4PA(dst +  0, dc);
        AV_WN4PA(dst +  4, dc);
        AV_WN4PA(dst +  8, dc);
        AV_WN4PA(dst + 12, dc);
        dst += stride;
    }
}

static void dc_left_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                            const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    pixel4 dc = PIXEL_SPLAT_X4
        ((left[0] + left[1] + left[2] + left[3] + left[4] + left[5] +
          left[6] + left[7] + left[8] + left[9] + left[10] + left[11] +
          left[12] + left[13] + left[14] + left[15] + left[16] + left[17] +
          left[18] + left[19] + left[20] + left[21] + left[22] + left[23] +
          left[24] + left[25] + left[26] + left[27] + left[28] + left[29] +
          left[30] + left[31] + 16) >> 5);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        AV_WN4PA(dst +  0, dc);
        AV_WN4PA(dst +  4, dc);
        AV_WN4PA(dst +  8, dc);
        AV_WN4PA(dst + 12, dc);
        AV_WN4PA(dst + 16, dc);
        AV_WN4PA(dst + 20, dc);
        AV_WN4PA(dst + 24, dc);
        AV_WN4PA(dst + 28, dc);
        dst += stride;
    }
}

static void dc_top_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4((top[0] + top[1] + top[2] + top[3] + 2) >> 2);

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, dc);
    AV_WN4PA(dst + stride * 1, dc);
    AV_WN4PA(dst + stride * 2, dc);
    AV_WN4PA(dst + stride * 3, dc);
}

static void dc_top_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4
        ((top[0] + top[1] + top[2] + top[3] +
          top[4] + top[5] + top[6] + top[7] + 4) >> 3);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        AV_WN4PA(dst + 0, dc);
        AV_WN4PA(dst + 4, dc);
        dst += stride;
    }
}

static void dc_top_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4
        ((top[0] + top[1] + top[2] + top[3] + top[4] + top[5] +
          top[6] + top[7] + top[8] + top[9] + top[10] + top[11] +
          top[12] + top[13] + top[14] + top[15] + 8) >> 4);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        AV_WN4PA(dst +  0, dc);
        AV_WN4PA(dst +  4, dc);
        AV_WN4PA(dst +  8, dc);
        AV_WN4PA(dst + 12, dc);
        dst += stride;
    }
}

static void dc_top_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    pixel4 dc = PIXEL_SPLAT_X4
        ((top[0] + top[1] + top[2] + top[3] + top[4] + top[5] +
          top[6] + top[7] + top[8] + top[9] + top[10] + top[11] +
          top[12] + top[13] + top[14] + top[15] + top[16] + top[17] +
          top[18] + top[19] + top[20] + top[21] + top[22] + top[23] +
          top[24] + top[25] + top[26] + top[27] + top[28] + top[29] +
          top[30] + top[31] + 16) >> 5);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        AV_WN4PA(dst +  0, dc);
        AV_WN4PA(dst +  4, dc);
        AV_WN4PA(dst +  8, dc);
        AV_WN4PA(dst + 12, dc);
        AV_WN4PA(dst + 16, dc);
        AV_WN4PA(dst + 20, dc);
        AV_WN4PA(dst + 24, dc);
        AV_WN4PA(dst + 28, dc);
        dst += stride;
    }
}

#endif /* BIT_DEPTH != 12 */

static void dc_128_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4(128 << (BIT_DEPTH - 8));

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, val);
    AV_WN4PA(dst + stride * 1, val);
    AV_WN4PA(dst + stride * 2, val);
    AV_WN4PA(dst + stride * 3, val);
}

static void dc_128_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4(128 << (BIT_DEPTH - 8));
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        AV_WN4PA(dst + 0, val);
        AV_WN4PA(dst + 4, val);
        dst += stride;
    }
}

static void dc_128_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4(128 << (BIT_DEPTH - 8));
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        AV_WN4PA(dst +  0, val);
        AV_WN4PA(dst +  4, val);
        AV_WN4PA(dst +  8, val);
        AV_WN4PA(dst + 12, val);
        dst += stride;
    }
}

static void dc_128_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4(128 << (BIT_DEPTH - 8));
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        AV_WN4PA(dst +  0, val);
        AV_WN4PA(dst +  4, val);
        AV_WN4PA(dst +  8, val);
        AV_WN4PA(dst + 12, val);
        AV_WN4PA(dst + 16, val);
        AV_WN4PA(dst + 20, val);
        AV_WN4PA(dst + 24, val);
        AV_WN4PA(dst + 28, val);
        dst += stride;
    }
}

static void dc_127_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) - 1);

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, val);
    AV_WN4PA(dst + stride * 1, val);
    AV_WN4PA(dst + stride * 2, val);
    AV_WN4PA(dst + stride * 3, val);}

static void dc_127_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) - 1);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        AV_WN4PA(dst + 0, val);
        AV_WN4PA(dst + 4, val);
        dst += stride;
    }
}

static void dc_127_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) - 1);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        AV_WN4PA(dst +  0, val);
        AV_WN4PA(dst +  4, val);
        AV_WN4PA(dst +  8, val);
        AV_WN4PA(dst + 12, val);
        dst += stride;
    }
}

static void dc_127_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) - 1);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        AV_WN4PA(dst +  0, val);
        AV_WN4PA(dst +  4, val);
        AV_WN4PA(dst +  8, val);
        AV_WN4PA(dst + 12, val);
        AV_WN4PA(dst + 16, val);
        AV_WN4PA(dst + 20, val);
        AV_WN4PA(dst + 24, val);
        AV_WN4PA(dst + 28, val);
        dst += stride;
    }
}

static void dc_129_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) + 1);

    stride /= sizeof(pixel);
    AV_WN4PA(dst + stride * 0, val);
    AV_WN4PA(dst + stride * 1, val);
    AV_WN4PA(dst + stride * 2, val);
    AV_WN4PA(dst + stride * 3, val);
}

static void dc_129_8x8_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) + 1);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 8; y++) {
        AV_WN4PA(dst + 0, val);
        AV_WN4PA(dst + 4, val);
        dst += stride;
    }
}

static void dc_129_16x16_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) + 1);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 16; y++) {
        AV_WN4PA(dst +  0, val);
        AV_WN4PA(dst +  4, val);
        AV_WN4PA(dst +  8, val);
        AV_WN4PA(dst + 12, val);
        dst += stride;
    }
}

static void dc_129_32x32_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    pixel4 val = PIXEL_SPLAT_X4((128 << (BIT_DEPTH - 8)) + 1);
    int y;

    stride /= sizeof(pixel);
    for (y = 0; y < 32; y++) {
        AV_WN4PA(dst +  0, val);
        AV_WN4PA(dst +  4, val);
        AV_WN4PA(dst +  8, val);
        AV_WN4PA(dst + 12, val);
        AV_WN4PA(dst + 16, val);
        AV_WN4PA(dst + 20, val);
        AV_WN4PA(dst + 24, val);
        AV_WN4PA(dst + 28, val);
        dst += stride;
    }
}

#if BIT_DEPTH != 12

#if BIT_DEPTH == 8
#define memset_bpc memset
#else
static inline void memset_bpc(uint16_t *dst, int val, int len) {
    int n;
    for (n = 0; n < len; n++) {
        dst[n] = val;
    }
}
#endif

#define DST(x, y) dst[(x) + (y) * stride]

static void diag_downleft_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                                const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    int a0 = top[0], a1 = top[1], a2 = top[2], a3 = top[3],
        a4 = top[4], a5 = top[5], a6 = top[6], a7 = top[7];

    stride /= sizeof(pixel);
    DST(0,0) = (a0 + a1 * 2 + a2 + 2) >> 2;
    DST(1,0) = DST(0,1) = (a1 + a2 * 2 + a3 + 2) >> 2;
    DST(2,0) = DST(1,1) = DST(0,2) = (a2 + a3 * 2 + a4 + 2) >> 2;
    DST(3,0) = DST(2,1) = DST(1,2) = DST(0,3) = (a3 + a4 * 2 + a5 + 2) >> 2;
    DST(3,1) = DST(2,2) = DST(1,3) = (a4 + a5 * 2 + a6 + 2) >> 2;
    DST(3,2) = DST(2,3) = (a5 + a6 * 2 + a7 + 2) >> 2;
    DST(3,3) = a7;  // note: this is different from vp8 and such
}

#define def_diag_downleft(size) \
static void diag_downleft_##size##x##size##_c(uint8_t *_dst, ptrdiff_t stride, \
                                              const uint8_t *left, const uint8_t *_top) \
{ \
    pixel *dst = (pixel *) _dst; \
    const pixel *top = (const pixel *) _top; \
    int i, j; \
    pixel v[size - 1]; \
\
    stride /= sizeof(pixel); \
    for (i = 0; i < size - 2; i++) \
        v[i] = (top[i] + top[i + 1] * 2 + top[i + 2] + 2) >> 2; \
    v[size - 2] = (top[size - 2] + top[size - 1] * 3 + 2) >> 2; \
\
    for (j = 0; j < size; j++) { \
        memcpy(dst + j*stride, v + j, (size - 1 - j) * sizeof(pixel)); \
        memset_bpc(dst + j*stride + size - 1 - j, top[size - 1], j + 1); \
    } \
}

def_diag_downleft(8)
def_diag_downleft(16)
def_diag_downleft(32)

static void diag_downright_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                                 const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    const pixel *left = (const pixel *) _left;
    int tl = top[-1], a0 = top[0], a1 = top[1], a2 = top[2], a3 = top[3],
        l0 = left[3], l1 = left[2], l2 = left[1], l3 = left[0];

    stride /= sizeof(pixel);
    DST(0,3) = (l1 + l2 * 2 + l3 + 2) >> 2;
    DST(0,2) = DST(1,3) = (l0 + l1 * 2 + l2 + 2) >> 2;
    DST(0,1) = DST(1,2) = DST(2,3) = (tl + l0 * 2 + l1 + 2) >> 2;
    DST(0,0) = DST(1,1) = DST(2,2) = DST(3,3) = (l0 + tl * 2 + a0 + 2) >> 2;
    DST(1,0) = DST(2,1) = DST(3,2) = (tl + a0 * 2 + a1 + 2) >> 2;
    DST(2,0) = DST(3,1) = (a0 + a1 * 2 + a2 + 2) >> 2;
    DST(3,0) = (a1 + a2 * 2 + a3 + 2) >> 2;
}

#define def_diag_downright(size) \
static void diag_downright_##size##x##size##_c(uint8_t *_dst, ptrdiff_t stride, \
                                               const uint8_t *_left, const uint8_t *_top) \
{ \
    pixel *dst = (pixel *) _dst; \
    const pixel *top = (const pixel *) _top; \
    const pixel *left = (const pixel *) _left; \
    int i, j; \
    pixel v[size + size - 1]; \
\
    stride /= sizeof(pixel); \
    for (i = 0; i < size - 2; i++) { \
        v[i           ] = (left[i] + left[i + 1] * 2 + left[i + 2] + 2) >> 2; \
        v[size + 1 + i] = (top[i]  + top[i + 1]  * 2 + top[i + 2]  + 2) >> 2; \
    } \
    v[size - 2] = (left[size - 2] + left[size - 1] * 2 + top[-1] + 2) >> 2; \
    v[size - 1] = (left[size - 1] + top[-1] * 2 + top[ 0] + 2) >> 2; \
    v[size    ] = (top[-1] + top[0]  * 2 + top[ 1] + 2) >> 2; \
\
    for (j = 0; j < size; j++) \
        memcpy(dst + j*stride, v + size - 1 - j, size * sizeof(pixel)); \
}

def_diag_downright(8)
def_diag_downright(16)
def_diag_downright(32)

static void vert_right_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                             const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    const pixel *left = (const pixel *) _left;
    int tl = top[-1], a0 = top[0], a1 = top[1], a2 = top[2], a3 = top[3],
        l0 = left[3], l1 = left[2], l2 = left[1];

    stride /= sizeof(pixel);
    DST(0,3) = (l0 + l1 * 2 + l2 + 2) >> 2;
    DST(0,2) = (tl + l0 * 2 + l1 + 2) >> 2;
    DST(0,0) = DST(1,2) = (tl + a0 + 1) >> 1;
    DST(0,1) = DST(1,3) = (l0 + tl * 2 + a0 + 2) >> 2;
    DST(1,0) = DST(2,2) = (a0 + a1 + 1) >> 1;
    DST(1,1) = DST(2,3) = (tl + a0 * 2 + a1 + 2) >> 2;
    DST(2,0) = DST(3,2) = (a1 + a2 + 1) >> 1;
    DST(2,1) = DST(3,3) = (a0 + a1 * 2 + a2 + 2) >> 2;
    DST(3,0) = (a2 + a3 + 1) >> 1;
    DST(3,1) = (a1 + a2 * 2 + a3 + 2) >> 2;
}

#define def_vert_right(size) \
static void vert_right_##size##x##size##_c(uint8_t *_dst, ptrdiff_t stride, \
                                           const uint8_t *_left, const uint8_t *_top) \
{ \
    pixel *dst = (pixel *) _dst; \
    const pixel *top = (const pixel *) _top; \
    const pixel *left = (const pixel *) _left; \
    int i, j; \
    pixel ve[size + size/2 - 1], vo[size + size/2 - 1]; \
\
    stride /= sizeof(pixel); \
    for (i = 0; i < size/2 - 2; i++) { \
        vo[i] = (left[i*2 + 3] + left[i*2 + 2] * 2 + left[i*2 + 1] + 2) >> 2; \
        ve[i] = (left[i*2 + 4] + left[i*2 + 3] * 2 + left[i*2 + 2] + 2) >> 2; \
    } \
    vo[size/2 - 2] = (left[size - 1] + left[size - 2] * 2 + left[size - 3] + 2) >> 2; \
    ve[size/2 - 2] = (top[-1] + left[size - 1] * 2 + left[size - 2] + 2) >> 2; \
\
    ve[size/2 - 1] = (top[-1] + top[0] + 1) >> 1; \
    vo[size/2 - 1] = (left[size - 1] + top[-1] * 2 + top[0] + 2) >> 2; \
    for (i = 0; i < size - 1; i++) { \
        ve[size/2 + i] = (top[i] + top[i + 1] + 1) >> 1; \
        vo[size/2 + i] = (top[i - 1] + top[i] * 2 + top[i + 1] + 2) >> 2; \
    } \
\
    for (j = 0; j < size / 2; j++) { \
        memcpy(dst +  j*2     *stride, ve + size/2 - 1 - j, size * sizeof(pixel)); \
        memcpy(dst + (j*2 + 1)*stride, vo + size/2 - 1 - j, size * sizeof(pixel)); \
    } \
}

def_vert_right(8)
def_vert_right(16)
def_vert_right(32)

static void hor_down_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                           const uint8_t *_left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    const pixel *left = (const pixel *) _left;
    int l0 = left[3], l1 = left[2], l2 = left[1], l3 = left[0],
        tl = top[-1], a0 = top[0], a1 = top[1], a2 = top[2];

    stride /= sizeof(pixel);
    DST(2,0) = (tl + a0 * 2 + a1 + 2) >> 2;
    DST(3,0) = (a0 + a1 * 2 + a2 + 2) >> 2;
    DST(0,0) = DST(2,1) = (tl + l0 + 1) >> 1;
    DST(1,0) = DST(3,1) = (a0 + tl * 2 + l0 + 2) >> 2;
    DST(0,1) = DST(2,2) = (l0 + l1 + 1) >> 1;
    DST(1,1) = DST(3,2) = (tl + l0 * 2 + l1 + 2) >> 2;
    DST(0,2) = DST(2,3) = (l1 + l2 + 1) >> 1;
    DST(1,2) = DST(3,3) = (l0 + l1 * 2 + l2 + 2) >> 2;
    DST(0,3) = (l2 + l3 + 1) >> 1;
    DST(1,3) = (l1 + l2 * 2 + l3 + 2) >> 2;
}

#define def_hor_down(size) \
static void hor_down_##size##x##size##_c(uint8_t *_dst, ptrdiff_t stride, \
                                         const uint8_t *_left, const uint8_t *_top) \
{ \
    pixel *dst = (pixel *) _dst; \
    const pixel *top = (const pixel *) _top; \
    const pixel *left = (const pixel *) _left; \
    int i, j; \
    pixel v[size * 3 - 2]; \
\
    stride /= sizeof(pixel); \
    for (i = 0; i < size - 2; i++) { \
        v[i*2       ] = (left[i + 1] + left[i + 0] + 1) >> 1; \
        v[i*2    + 1] = (left[i + 2] + left[i + 1] * 2 + left[i + 0] + 2) >> 2; \
        v[size*2 + i] = (top[i - 1] + top[i] * 2 + top[i + 1] + 2) >> 2; \
    } \
    v[size*2 - 2] = (top[-1] + left[size - 1] + 1) >> 1; \
    v[size*2 - 4] = (left[size - 1] + left[size - 2] + 1) >> 1; \
    v[size*2 - 1] = (top[0]  + top[-1] * 2 + left[size - 1] + 2) >> 2; \
    v[size*2 - 3] = (top[-1] + left[size - 1] * 2 + left[size - 2] + 2) >> 2; \
\
    for (j = 0; j < size; j++) \
        memcpy(dst + j*stride, v + size*2 - 2 - j*2, size * sizeof(pixel)); \
}

def_hor_down(8)
def_hor_down(16)
def_hor_down(32)

static void vert_left_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                            const uint8_t *left, const uint8_t *_top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *top = (const pixel *) _top;
    int a0 = top[0], a1 = top[1], a2 = top[2], a3 = top[3],
        a4 = top[4], a5 = top[5], a6 = top[6];

    stride /= sizeof(pixel);
    DST(0,0) = (a0 + a1 + 1) >> 1;
    DST(0,1) = (a0 + a1 * 2 + a2 + 2) >> 2;
    DST(1,0) = DST(0,2) = (a1 + a2 + 1) >> 1;
    DST(1,1) = DST(0,3) = (a1 + a2 * 2 + a3 + 2) >> 2;
    DST(2,0) = DST(1,2) = (a2 + a3 + 1) >> 1;
    DST(2,1) = DST(1,3) = (a2 + a3 * 2 + a4 + 2) >> 2;
    DST(3,0) = DST(2,2) = (a3 + a4 + 1) >> 1;
    DST(3,1) = DST(2,3) = (a3 + a4 * 2 + a5 + 2) >> 2;
    DST(3,2) = (a4 + a5 + 1) >> 1;
    DST(3,3) = (a4 + a5 * 2 + a6 + 2) >> 2;
}

#define def_vert_left(size) \
static void vert_left_##size##x##size##_c(uint8_t *_dst, ptrdiff_t stride, \
                                          const uint8_t *left, const uint8_t *_top) \
{ \
    pixel *dst = (pixel *) _dst; \
    const pixel *top = (const pixel *) _top; \
    int i, j; \
    pixel ve[size - 1], vo[size - 1]; \
\
    stride /= sizeof(pixel); \
    for (i = 0; i < size - 2; i++) { \
        ve[i] = (top[i] + top[i + 1] + 1) >> 1; \
        vo[i] = (top[i] + top[i + 1] * 2 + top[i + 2] + 2) >> 2; \
    } \
    ve[size - 2] = (top[size - 2] + top[size - 1] + 1) >> 1; \
    vo[size - 2] = (top[size - 2] + top[size - 1] * 3 + 2) >> 2; \
\
    for (j = 0; j < size / 2; j++) { \
        memcpy(dst +  j*2      * stride, ve + j, (size - j - 1) * sizeof(pixel)); \
        memset_bpc(dst +  j*2      * stride + size - j - 1, top[size - 1], j + 1); \
        memcpy(dst + (j*2 + 1) * stride, vo + j, (size - j - 1) * sizeof(pixel)); \
        memset_bpc(dst + (j*2 + 1) * stride + size - j - 1, top[size - 1], j + 1); \
    } \
}

def_vert_left(8)
def_vert_left(16)
def_vert_left(32)

static void hor_up_4x4_c(uint8_t *_dst, ptrdiff_t stride,
                         const uint8_t *_left, const uint8_t *top)
{
    pixel *dst = (pixel *) _dst;
    const pixel *left = (const pixel *) _left;
    int l0 = left[0], l1 = left[1], l2 = left[2], l3 = left[3];

    stride /= sizeof(pixel);
    DST(0,0) = (l0 + l1 + 1) >> 1;
    DST(1,0) = (l0 + l1 * 2 + l2 + 2) >> 2;
    DST(0,1) = DST(2,0) = (l1 + l2 + 1) >> 1;
    DST(1,1) = DST(3,0) = (l1 + l2 * 2 + l3 + 2) >> 2;
    DST(0,2) = DST(2,1) = (l2 + l3 + 1) >> 1;
    DST(1,2) = DST(3,1) = (l2 + l3 * 3 + 2) >> 2;
    DST(0,3) = DST(1,3) = DST(2,2) = DST(2,3) = DST(3,2) = DST(3,3) = l3;
}

#define def_hor_up(size) \
static void hor_up_##size##x##size##_c(uint8_t *_dst, ptrdiff_t stride, \
                                       const uint8_t *_left, const uint8_t *top) \
{ \
    pixel *dst = (pixel *) _dst; \
    const pixel *left = (const pixel *) _left; \
    int i, j; \
    pixel v[size*2 - 2]; \
\
    stride /= sizeof(pixel); \
    for (i = 0; i < size - 2; i++) { \
        v[i*2    ] = (left[i] + left[i + 1] + 1) >> 1; \
        v[i*2 + 1] = (left[i] + left[i + 1] * 2 + left[i + 2] + 2) >> 2; \
    } \
    v[size*2 - 4] = (left[size - 2] + left[size - 1] + 1) >> 1; \
    v[size*2 - 3] = (left[size - 2] + left[size - 1] * 3 + 2) >> 2; \
\
    for (j = 0; j < size / 2; j++) \
        memcpy(dst + j*stride, v + j*2, size * sizeof(pixel)); \
    for (j = size / 2; j < size; j++) { \
        memcpy(dst + j*stride, v + j*2, (size*2 - 2 - j*2) * sizeof(pixel)); \
        memset_bpc(dst + j*stride + size*2 - 2 - j*2, left[size - 1], \
                   2 + j*2 - size); \
    } \
}

def_hor_up(8)
def_hor_up(16)
def_hor_up(32)

#undef DST

#endif /* BIT_DEPTH != 12 */

#if BIT_DEPTH != 8
void ff_vp9dsp_intrapred_init_10(VP9DSPContext *dsp);
#endif
#if BIT_DEPTH != 10
static
#endif
av_cold void FUNC(ff_vp9dsp_intrapred_init)(VP9DSPContext *dsp)
{
#define init_intra_pred_bd_aware(tx, sz) \
    dsp->intra_pred[tx][TM_VP8_PRED]          = tm_##sz##_c; \
    dsp->intra_pred[tx][DC_128_PRED]          = dc_128_##sz##_c; \
    dsp->intra_pred[tx][DC_127_PRED]          = dc_127_##sz##_c; \
    dsp->intra_pred[tx][DC_129_PRED]          = dc_129_##sz##_c

#if BIT_DEPTH == 12
    ff_vp9dsp_intrapred_init_10(dsp);
#define init_intra_pred(tx, sz) \
    init_intra_pred_bd_aware(tx, sz)
#else
    #define init_intra_pred(tx, sz) \
    dsp->intra_pred[tx][VERT_PRED]            = vert_##sz##_c; \
    dsp->intra_pred[tx][HOR_PRED]             = hor_##sz##_c; \
    dsp->intra_pred[tx][DC_PRED]              = dc_##sz##_c; \
    dsp->intra_pred[tx][DIAG_DOWN_LEFT_PRED]  = diag_downleft_##sz##_c; \
    dsp->intra_pred[tx][DIAG_DOWN_RIGHT_PRED] = diag_downright_##sz##_c; \
    dsp->intra_pred[tx][VERT_RIGHT_PRED]      = vert_right_##sz##_c; \
    dsp->intra_pred[tx][HOR_DOWN_PRED]        = hor_down_##sz##_c; \
    dsp->intra_pred[tx][VERT_LEFT_PRED]       = vert_left_##sz##_c; \
    dsp->intra_pred[tx][HOR_UP_PRED]          = hor_up_##sz##_c; \
    dsp->intra_pred[tx][LEFT_DC_PRED]         = dc_left_##sz##_c; \
    dsp->intra_pred[tx][TOP_DC_PRED]          = dc_top_##sz##_c; \
    init_intra_pred_bd_aware(tx, sz)
#endif

    init_intra_pred(TX_4X4,   4x4);
    init_intra_pred(TX_8X8,   8x8);
    init_intra_pred(TX_16X16, 16x16);
    init_intra_pred(TX_32X32, 32x32);

#undef init_intra_pred
#undef init_intra_pred_bd_aware
}

#define itxfm_wrapper(type_a, type_b, sz, bits, has_dconly) \
static void type_a##_##type_b##_##sz##x##sz##_add_c(uint8_t *_dst, \
                                                    ptrdiff_t stride, \
                                                    int16_t *_block, int eob) \
{ \
    int i, j; \
    pixel *dst = (pixel *) _dst; \
    dctcoef *block = (dctcoef *) _block, tmp[sz * sz], out[sz]; \
\
    stride /= sizeof(pixel); \
    if (has_dconly && eob == 1) { \
        const int t  = ((((dctint) block[0] * 11585 + (1 << 13)) >> 14) \
                                            * 11585 + (1 << 13)) >> 14; \
        block[0] = 0; \
        for (i = 0; i < sz; i++) { \
            for (j = 0; j < sz; j++) \
                dst[j * stride] = av_clip_pixel(dst[j * stride] + \
                                                (bits ? \
                                                 (int)(t + (1U << (bits - 1))) >> bits : \
                                                 t)); \
            dst++; \
        } \
        return; \
    } \
\
    for (i = 0; i < sz; i++) \
        type_a##sz##_1d(block + i, sz, tmp + i * sz, 0); \
    memset(block, 0, sz * sz * sizeof(*block)); \
    for (i = 0; i < sz; i++) { \
        type_b##sz##_1d(tmp + i, sz, out, 1); \
        for (j = 0; j < sz; j++) \
            dst[j * stride] = av_clip_pixel(dst[j * stride] + \
                                            (bits ? \
                                             (int)(out[j] + (1U << (bits - 1))) >> bits : \
                                             out[j])); \
        dst++; \
    } \
}

#define itxfm_wrap(sz, bits) \
itxfm_wrapper(idct,  idct,  sz, bits, 1) \
itxfm_wrapper(iadst, idct,  sz, bits, 0) \
itxfm_wrapper(idct,  iadst, sz, bits, 0) \
itxfm_wrapper(iadst, iadst, sz, bits, 0)

#define IN(x) ((dctint) in[(x) * stride])

static av_always_inline void idct4_1d(const dctcoef *in, ptrdiff_t stride,
                                      dctcoef *out, int pass)
{
    dctint t0, t1, t2, t3;

    t0 = ((IN(0) + IN(2)) * 11585 + (1 << 13)) >> 14;
    t1 = ((IN(0) - IN(2)) * 11585 + (1 << 13)) >> 14;
    t2 = (IN(1) *  6270 - IN(3) * 15137 + (1 << 13)) >> 14;
    t3 = (IN(1) * 15137 + IN(3) *  6270 + (1 << 13)) >> 14;

    out[0] = t0 + t3;
    out[1] = t1 + t2;
    out[2] = t1 - t2;
    out[3] = t0 - t3;
}

static av_always_inline void iadst4_1d(const dctcoef *in, ptrdiff_t stride,
                                       dctcoef *out, int pass)
{
    dctint t0, t1, t2, t3;

    t0 =  5283 * IN(0) + 15212 * IN(2) +  9929 * IN(3);
    t1 =  9929 * IN(0) -  5283 * IN(2) - 15212 * IN(3);
    t2 = 13377 * (IN(0) - IN(2) + IN(3));
    t3 = 13377 * IN(1);

    out[0] = (t0 + t3      + (1 << 13)) >> 14;
    out[1] = (t1 + t3      + (1 << 13)) >> 14;
    out[2] = (t2           + (1 << 13)) >> 14;
    out[3] = (t0 + t1 - t3 + (1 << 13)) >> 14;
}

itxfm_wrap(4, 4)

static av_always_inline void idct8_1d(const dctcoef *in, ptrdiff_t stride,
                                      dctcoef *out, int pass)
{
    dctint t0, t0a, t1, t1a, t2, t2a, t3, t3a, t4, t4a, t5, t5a, t6, t6a, t7, t7a;

    t0a = ((IN(0) + IN(4)) * 11585 + (1 << 13)) >> 14;
    t1a = ((IN(0) - IN(4)) * 11585 + (1 << 13)) >> 14;
    t2a = (IN(2) *  6270 - IN(6) * 15137 + (1 << 13)) >> 14;
    t3a = (IN(2) * 15137 + IN(6) *  6270 + (1 << 13)) >> 14;
    t4a = (IN(1) *  3196 - IN(7) * 16069 + (1 << 13)) >> 14;
    t5a = (IN(5) * 13623 - IN(3) *  9102 + (1 << 13)) >> 14;
    t6a = (IN(5) *  9102 + IN(3) * 13623 + (1 << 13)) >> 14;
    t7a = (IN(1) * 16069 + IN(7) *  3196 + (1 << 13)) >> 14;

    t0  = t0a + t3a;
    t1  = t1a + t2a;
    t2  = t1a - t2a;
    t3  = t0a - t3a;
    t4  = t4a + t5a;
    t5a = t4a - t5a;
    t7  = t7a + t6a;
    t6a = t7a - t6a;

    t5  = ((t6a - t5a) * 11585 + (1 << 13)) >> 14;
    t6  = ((t6a + t5a) * 11585 + (1 << 13)) >> 14;

    out[0] = t0 + t7;
    out[1] = t1 + t6;
    out[2] = t2 + t5;
    out[3] = t3 + t4;
    out[4] = t3 - t4;
    out[5] = t2 - t5;
    out[6] = t1 - t6;
    out[7] = t0 - t7;
}

static av_always_inline void iadst8_1d(const dctcoef *in, ptrdiff_t stride,
                                       dctcoef *out, int pass)
{
    dctint t0, t0a, t1, t1a, t2, t2a, t3, t3a, t4, t4a, t5, t5a, t6, t6a, t7, t7a;

    t0a = 16305 * IN(7) +  1606 * IN(0);
    t1a =  1606 * IN(7) - 16305 * IN(0);
    t2a = 14449 * IN(5) +  7723 * IN(2);
    t3a =  7723 * IN(5) - 14449 * IN(2);
    t4a = 10394 * IN(3) + 12665 * IN(4);
    t5a = 12665 * IN(3) - 10394 * IN(4);
    t6a =  4756 * IN(1) + 15679 * IN(6);
    t7a = 15679 * IN(1) -  4756 * IN(6);

    t0 = (t0a + t4a + (1 << 13)) >> 14;
    t1 = (t1a + t5a + (1 << 13)) >> 14;
    t2 = (t2a + t6a + (1 << 13)) >> 14;
    t3 = (t3a + t7a + (1 << 13)) >> 14;
    t4 = (t0a - t4a + (1 << 13)) >> 14;
    t5 = (t1a - t5a + (1 << 13)) >> 14;
    t6 = (t2a - t6a + (1 << 13)) >> 14;
    t7 = (t3a - t7a + (1 << 13)) >> 14;

    t4a = 15137U * t4 +  6270U * t5;
    t5a =  6270U * t4 - 15137U * t5;
    t6a = 15137U * t7 -  6270U * t6;
    t7a =  6270U * t7 + 15137U * t6;

    out[0] =   t0 + t2;
    out[7] = -(t1 + t3);
    t2     =   t0 - t2;
    t3     =   t1 - t3;

    out[1] = -((dctint)((1U << 13) + t4a + t6a) >> 14);
    out[6] =   (dctint)((1U << 13) + t5a + t7a) >> 14;
    t6     =   (dctint)((1U << 13) + t4a - t6a) >> 14;
    t7     =   (dctint)((1U << 13) + t5a - t7a) >> 14;

    out[3] = -((dctint)((t2 + t3) * 11585U + (1 << 13)) >> 14);
    out[4] =   (dctint)((t2 - t3) * 11585U + (1 << 13)) >> 14;
    out[2] =   (dctint)((t6 + t7) * 11585U + (1 << 13)) >> 14;
    out[5] = -((dctint)((t6 - t7) * 11585U + (1 << 13)) >> 14);
}

itxfm_wrap(8, 5)

static av_always_inline void idct16_1d(const dctcoef *in, ptrdiff_t stride,
                                       dctcoef *out, int pass)
{
    dctint t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15;
    dctint t0a, t1a, t2a, t3a, t4a, t5a, t6a, t7a;
    dctint t8a, t9a, t10a, t11a, t12a, t13a, t14a, t15a;

    t0a  = (dctint)((IN(0) + IN(8)) * 11585U + (1 << 13)) >> 14;
    t1a  = (dctint)((IN(0) - IN(8)) * 11585U + (1 << 13)) >> 14;
    t2a  = (dctint)(IN(4)  *  6270U - IN(12) * 15137U + (1 << 13)) >> 14;
    t3a  = (dctint)(IN(4)  * 15137U + IN(12) *  6270U + (1 << 13)) >> 14;
    t4a  = (dctint)(IN(2)  *  3196U - IN(14) * 16069U + (1 << 13)) >> 14;
    t7a  = (dctint)(IN(2)  * 16069U + IN(14) *  3196U + (1 << 13)) >> 14;
    t5a  = (dctint)(IN(10) * 13623U - IN(6)  *  9102U + (1 << 13)) >> 14;
    t6a  = (dctint)(IN(10) *  9102U + IN(6)  * 13623U + (1 << 13)) >> 14;
    t8a  = (dctint)(IN(1)  *  1606U - IN(15) * 16305U + (1 << 13)) >> 14;
    t15a = (dctint)(IN(1)  * 16305U + IN(15) *  1606U + (1 << 13)) >> 14;
    t9a  = (dctint)(IN(9)  * 12665U - IN(7)  * 10394U + (1 << 13)) >> 14;
    t14a = (dctint)(IN(9)  * 10394U + IN(7)  * 12665U + (1 << 13)) >> 14;
    t10a = (dctint)(IN(5)  *  7723U - IN(11) * 14449U + (1 << 13)) >> 14;
    t13a = (dctint)(IN(5)  * 14449U + IN(11) *  7723U + (1 << 13)) >> 14;
    t11a = (dctint)(IN(13) * 15679U - IN(3)  *  4756U + (1 << 13)) >> 14;
    t12a = (dctint)(IN(13) *  4756U + IN(3)  * 15679U + (1 << 13)) >> 14;

    t0  = t0a  + t3a;
    t1  = t1a  + t2a;
    t2  = t1a  - t2a;
    t3  = t0a  - t3a;
    t4  = t4a  + t5a;
    t5  = t4a  - t5a;
    t6  = t7a  - t6a;
    t7  = t7a  + t6a;
    t8  = t8a  + t9a;
    t9  = t8a  - t9a;
    t10 = t11a - t10a;
    t11 = t11a + t10a;
    t12 = t12a + t13a;
    t13 = t12a - t13a;
    t14 = t15a - t14a;
    t15 = t15a + t14a;

    t5a  = (dctint)((t6 - t5) * 11585U + (1 << 13)) >> 14;
    t6a  = (dctint)((t6 + t5) * 11585U + (1 << 13)) >> 14;
    t9a  = (dctint)(  t14 *  6270U - t9  * 15137U  + (1 << 13)) >> 14;
    t14a = (dctint)(  t14 * 15137U + t9  *  6270U  + (1 << 13)) >> 14;
    t10a = (dctint)(-(t13 * 15137U + t10 *  6270U) + (1 << 13)) >> 14;
    t13a = (dctint)(  t13 *  6270U - t10 * 15137U  + (1 << 13)) >> 14;

    t0a  = t0   + t7;
    t1a  = t1   + t6a;
    t2a  = t2   + t5a;
    t3a  = t3   + t4;
    t4   = t3   - t4;
    t5   = t2   - t5a;
    t6   = t1   - t6a;
    t7   = t0   - t7;
    t8a  = t8   + t11;
    t9   = t9a  + t10a;
    t10  = t9a  - t10a;
    t11a = t8   - t11;
    t12a = t15  - t12;
    t13  = t14a - t13a;
    t14  = t14a + t13a;
    t15a = t15  + t12;

    t10a = (dctint)((t13  - t10)  * 11585U + (1 << 13)) >> 14;
    t13a = (dctint)((t13  + t10)  * 11585U + (1 << 13)) >> 14;
    t11  = (dctint)((t12a - t11a) * 11585U + (1 << 13)) >> 14;
    t12  = (dctint)((t12a + t11a) * 11585U + (1 << 13)) >> 14;

    out[ 0] = t0a + t15a;
    out[ 1] = t1a + t14;
    out[ 2] = t2a + t13a;
    out[ 3] = t3a + t12;
    out[ 4] = t4  + t11;
    out[ 5] = t5  + t10a;
    out[ 6] = t6  + t9;
    out[ 7] = t7  + t8a;
    out[ 8] = t7  - t8a;
    out[ 9] = t6  - t9;
    out[10] = t5  - t10a;
    out[11] = t4  - t11;
    out[12] = t3a - t12;
    out[13] = t2a - t13a;
    out[14] = t1a - t14;
    out[15] = t0a - t15a;
}

static av_always_inline void iadst16_1d(const dctcoef *in, ptrdiff_t stride,
                                        dctcoef *out, int pass)
{
    dctint t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15;
    dctint t0a, t1a, t2a, t3a, t4a, t5a, t6a, t7a;
    dctint t8a, t9a, t10a, t11a, t12a, t13a, t14a, t15a;

    t0  = IN(15) * 16364U + IN(0)  *   804U;
    t1  = IN(15) *   804U - IN(0)  * 16364U;
    t2  = IN(13) * 15893U + IN(2)  *  3981U;
    t3  = IN(13) *  3981U - IN(2)  * 15893U;
    t4  = IN(11) * 14811U + IN(4)  *  7005U;
    t5  = IN(11) *  7005U - IN(4)  * 14811U;
    t6  = IN(9)  * 13160U + IN(6)  *  9760U;
    t7  = IN(9)  *  9760U - IN(6)  * 13160U;
    t8  = IN(7)  * 11003U + IN(8)  * 12140U;
    t9  = IN(7)  * 12140U - IN(8)  * 11003U;
    t10 = IN(5)  *  8423U + IN(10) * 14053U;
    t11 = IN(5)  * 14053U - IN(10) *  8423U;
    t12 = IN(3)  *  5520U + IN(12) * 15426U;
    t13 = IN(3)  * 15426U - IN(12) *  5520U;
    t14 = IN(1)  *  2404U + IN(14) * 16207U;
    t15 = IN(1)  * 16207U - IN(14) *  2404U;

    t0a  = (dctint)((1U << 13) + t0 + t8 ) >> 14;
    t1a  = (dctint)((1U << 13) + t1 + t9 ) >> 14;
    t2a  = (dctint)((1U << 13) + t2 + t10) >> 14;
    t3a  = (dctint)((1U << 13) + t3 + t11) >> 14;
    t4a  = (dctint)((1U << 13) + t4 + t12) >> 14;
    t5a  = (dctint)((1U << 13) + t5 + t13) >> 14;
    t6a  = (dctint)((1U << 13) + t6 + t14) >> 14;
    t7a  = (dctint)((1U << 13) + t7 + t15) >> 14;
    t8a  = (dctint)((1U << 13) + t0 - t8 ) >> 14;
    t9a  = (dctint)((1U << 13) + t1 - t9 ) >> 14;
    t10a = (dctint)((1U << 13) + t2 - t10) >> 14;
    t11a = (dctint)((1U << 13) + t3 - t11) >> 14;
    t12a = (dctint)((1U << 13) + t4 - t12) >> 14;
    t13a = (dctint)((1U << 13) + t5 - t13) >> 14;
    t14a = (dctint)((1U << 13) + t6 - t14) >> 14;
    t15a = (dctint)((1U << 13) + t7 - t15) >> 14;

    t8   = t8a  * 16069U + t9a  *  3196U;
    t9   = t8a  *  3196U - t9a  * 16069U;
    t10  = t10a *  9102U + t11a * 13623U;
    t11  = t10a * 13623U - t11a *  9102U;
    t12  = t13a * 16069U - t12a *  3196U;
    t13  = t13a *  3196U + t12a * 16069U;
    t14  = t15a *  9102U - t14a * 13623U;
    t15  = t15a * 13623U + t14a *  9102U;

    t0   = t0a + t4a;
    t1   = t1a + t5a;
    t2   = t2a + t6a;
    t3   = t3a + t7a;
    t4   = t0a - t4a;
    t5   = t1a - t5a;
    t6   = t2a - t6a;
    t7   = t3a - t7a;
    t8a  = (dctint)((1U << 13) + t8  + t12) >> 14;
    t9a  = (dctint)((1U << 13) + t9  + t13) >> 14;
    t10a = (dctint)((1U << 13) + t10 + t14) >> 14;
    t11a = (dctint)((1U << 13) + t11 + t15) >> 14;
    t12a = (dctint)((1U << 13) + t8  - t12) >> 14;
    t13a = (dctint)((1U << 13) + t9  - t13) >> 14;
    t14a = (dctint)((1U << 13) + t10 - t14) >> 14;
    t15a = (dctint)((1U << 13) + t11 - t15) >> 14;

    t4a  = t4 * 15137U + t5 *  6270U;
    t5a  = t4 *  6270U - t5 * 15137U;
    t6a  = t7 * 15137U - t6 *  6270U;
    t7a  = t7 *  6270U + t6 * 15137U;
    t12  = t12a * 15137U + t13a *  6270U;
    t13  = t12a *  6270U - t13a * 15137U;
    t14  = t15a * 15137U - t14a *  6270U;
    t15  = t15a *  6270U + t14a * 15137U;

    out[ 0] =   t0 + t2;
    out[15] = -(t1 + t3);
    t2a     =   t0 - t2;
    t3a     =   t1 - t3;
    out[ 3] = -((dctint)((1U << 13) + t4a + t6a) >> 14);
    out[12] =   (dctint)((1U << 13) + t5a + t7a) >> 14;
    t6      =   (dctint)((1U << 13) + t4a - t6a) >> 14;
    t7      =   (dctint)((1U << 13) + t5a - t7a) >> 14;
    out[ 1] = -(t8a + t10a);
    out[14] =   t9a + t11a;
    t10     =   t8a - t10a;
    t11     =   t9a - t11a;
    out[ 2] =   (dctint)((1U << 13) + t12 + t14) >> 14;
    out[13] = -((dctint)((1U << 13) + t13 + t15) >> 14);
    t14a    =   (dctint)((1U << 13) + t12 - t14) >> 14;
    t15a    =   (dctint)((1U << 13) + t13 - t15) >> 14;

    out[ 7] = (dctint)(-(t2a  + t3a)  * 11585U  + (1 << 13)) >> 14;
    out[ 8] = (dctint)( (t2a  - t3a)  * 11585U  + (1 << 13)) >> 14;
    out[ 4] = (dctint)( (t7   + t6)   * 11585U  + (1 << 13)) >> 14;
    out[11] = (dctint)( (t7   - t6)   * 11585U  + (1 << 13)) >> 14;
    out[ 6] = (dctint)( (t11  + t10)  * 11585U  + (1 << 13)) >> 14;
    out[ 9] = (dctint)( (t11  - t10)  * 11585U  + (1 << 13)) >> 14;
    out[ 5] = (dctint)(-(t14a + t15a) * 11585U  + (1 << 13)) >> 14;
    out[10] = (dctint)( (t14a - t15a) * 11585U  + (1 << 13)) >> 14;
}

itxfm_wrap(16, 6)

static av_always_inline void idct32_1d(const dctcoef *in, ptrdiff_t stride,
                                       dctcoef *out, int pass)
{
    dctint t0a  = (dctint)((IN(0) + IN(16)) * 11585U         + (1 << 13)) >> 14;
    dctint t1a  = (dctint)((IN(0) - IN(16)) * 11585U         + (1 << 13)) >> 14;
    dctint t2a  = (dctint)(IN( 8) *  6270U - IN(24) * 15137U + (1 << 13)) >> 14;
    dctint t3a  = (dctint)(IN( 8) * 15137U + IN(24) *  6270U + (1 << 13)) >> 14;
    dctint t4a  = (dctint)(IN( 4) *  3196U - IN(28) * 16069U + (1 << 13)) >> 14;
    dctint t7a  = (dctint)(IN( 4) * 16069U + IN(28) *  3196U + (1 << 13)) >> 14;
    dctint t5a  = (dctint)(IN(20) * 13623U - IN(12) *  9102U + (1 << 13)) >> 14;
    dctint t6a  = (dctint)(IN(20) *  9102U + IN(12) * 13623U + (1 << 13)) >> 14;
    dctint t8a  = (dctint)(IN( 2) *  1606U - IN(30) * 16305U + (1 << 13)) >> 14;
    dctint t15a = (dctint)(IN( 2) * 16305U + IN(30) *  1606U + (1 << 13)) >> 14;
    dctint t9a  = (dctint)(IN(18) * 12665U - IN(14) * 10394U + (1 << 13)) >> 14;
    dctint t14a = (dctint)(IN(18) * 10394U + IN(14) * 12665U + (1 << 13)) >> 14;
    dctint t10a = (dctint)(IN(10) *  7723U - IN(22) * 14449U + (1 << 13)) >> 14;
    dctint t13a = (dctint)(IN(10) * 14449U + IN(22) *  7723U + (1 << 13)) >> 14;
    dctint t11a = (dctint)(IN(26) * 15679U - IN( 6) *  4756U + (1 << 13)) >> 14;
    dctint t12a = (dctint)(IN(26) *  4756U + IN( 6) * 15679U + (1 << 13)) >> 14;
    dctint t16a = (dctint)(IN( 1) *   804U - IN(31) * 16364U + (1 << 13)) >> 14;
    dctint t31a = (dctint)(IN( 1) * 16364U + IN(31) *   804U + (1 << 13)) >> 14;
    dctint t17a = (dctint)(IN(17) * 12140U - IN(15) * 11003U + (1 << 13)) >> 14;
    dctint t30a = (dctint)(IN(17) * 11003U + IN(15) * 12140U + (1 << 13)) >> 14;
    dctint t18a = (dctint)(IN( 9) *  7005U - IN(23) * 14811U + (1 << 13)) >> 14;
    dctint t29a = (dctint)(IN( 9) * 14811U + IN(23) *  7005U + (1 << 13)) >> 14;
    dctint t19a = (dctint)(IN(25) * 15426U - IN( 7) *  5520U + (1 << 13)) >> 14;
    dctint t28a = (dctint)(IN(25) *  5520U + IN( 7) * 15426U + (1 << 13)) >> 14;
    dctint t20a = (dctint)(IN( 5) *  3981U - IN(27) * 15893U + (1 << 13)) >> 14;
    dctint t27a = (dctint)(IN( 5) * 15893U + IN(27) *  3981U + (1 << 13)) >> 14;
    dctint t21a = (dctint)(IN(21) * 14053U - IN(11) *  8423U + (1 << 13)) >> 14;
    dctint t26a = (dctint)(IN(21) *  8423U + IN(11) * 14053U + (1 << 13)) >> 14;
    dctint t22a = (dctint)(IN(13) *  9760U - IN(19) * 13160U + (1 << 13)) >> 14;
    dctint t25a = (dctint)(IN(13) * 13160U + IN(19) *  9760U + (1 << 13)) >> 14;
    dctint t23a = (dctint)(IN(29) * 16207U - IN( 3) *  2404U + (1 << 13)) >> 14;
    dctint t24a = (dctint)(IN(29) *  2404U + IN( 3) * 16207U + (1 << 13)) >> 14;

    dctint t0  = t0a  + t3a;
    dctint t1  = t1a  + t2a;
    dctint t2  = t1a  - t2a;
    dctint t3  = t0a  - t3a;
    dctint t4  = t4a  + t5a;
    dctint t5  = t4a  - t5a;
    dctint t6  = t7a  - t6a;
    dctint t7  = t7a  + t6a;
    dctint t8  = t8a  + t9a;
    dctint t9  = t8a  - t9a;
    dctint t10 = t11a - t10a;
    dctint t11 = t11a + t10a;
    dctint t12 = t12a + t13a;
    dctint t13 = t12a - t13a;
    dctint t14 = t15a - t14a;
    dctint t15 = t15a + t14a;
    dctint t16 = t16a + t17a;
    dctint t17 = t16a - t17a;
    dctint t18 = t19a - t18a;
    dctint t19 = t19a + t18a;
    dctint t20 = t20a + t21a;
    dctint t21 = t20a - t21a;
    dctint t22 = t23a - t22a;
    dctint t23 = t23a + t22a;
    dctint t24 = t24a + t25a;
    dctint t25 = t24a - t25a;
    dctint t26 = t27a - t26a;
    dctint t27 = t27a + t26a;
    dctint t28 = t28a + t29a;
    dctint t29 = t28a - t29a;
    dctint t30 = t31a - t30a;
    dctint t31 = t31a + t30a;

    t5a  = (dctint)((t6 - t5) * 11585U             + (1 << 13)) >> 14;
    t6a  = (dctint)((t6 + t5) * 11585U             + (1 << 13)) >> 14;
    t9a  = (dctint)(  t14 *  6270U - t9  * 15137U  + (1 << 13)) >> 14;
    t14a = (dctint)(  t14 * 15137U + t9  *  6270U  + (1 << 13)) >> 14;
    t10a = (dctint)(-(t13 * 15137U + t10 *  6270U) + (1 << 13)) >> 14;
    t13a = (dctint)(  t13 *  6270U - t10 * 15137U  + (1 << 13)) >> 14;
    t17a = (dctint)(  t30 *  3196U - t17 * 16069U  + (1 << 13)) >> 14;
    t30a = (dctint)(  t30 * 16069U + t17 *  3196U  + (1 << 13)) >> 14;
    t18a = (dctint)(-(t29 * 16069U + t18 *  3196U) + (1 << 13)) >> 14;
    t29a = (dctint)(  t29 *  3196U - t18 * 16069U  + (1 << 13)) >> 14;
    t21a = (dctint)(  t26 * 13623U - t21 *  9102U  + (1 << 13)) >> 14;
    t26a = (dctint)(  t26 *  9102U + t21 * 13623U  + (1 << 13)) >> 14;
    t22a = (dctint)(-(t25 *  9102U + t22 * 13623U) + (1 << 13)) >> 14;
    t25a = (dctint)(  t25 * 13623U - t22 *  9102U  + (1 << 13)) >> 14;

    t0a  = t0   + t7;
    t1a  = t1   + t6a;
    t2a  = t2   + t5a;
    t3a  = t3   + t4;
    t4a  = t3   - t4;
    t5   = t2   - t5a;
    t6   = t1   - t6a;
    t7a  = t0   - t7;
    t8a  = t8   + t11;
    t9   = t9a  + t10a;
    t10  = t9a  - t10a;
    t11a = t8   - t11;
    t12a = t15  - t12;
    t13  = t14a - t13a;
    t14  = t14a + t13a;
    t15a = t15  + t12;
    t16a = t16  + t19;
    t17  = t17a + t18a;
    t18  = t17a - t18a;
    t19a = t16  - t19;
    t20a = t23  - t20;
    t21  = t22a - t21a;
    t22  = t22a + t21a;
    t23a = t23  + t20;
    t24a = t24  + t27;
    t25  = t25a + t26a;
    t26  = t25a - t26a;
    t27a = t24  - t27;
    t28a = t31  - t28;
    t29  = t30a - t29a;
    t30  = t30a + t29a;
    t31a = t31  + t28;

    t10a = (dctint)((t13  - t10)  * 11585U           + (1 << 13)) >> 14;
    t13a = (dctint)((t13  + t10)  * 11585U           + (1 << 13)) >> 14;
    t11  = (dctint)((t12a - t11a) * 11585U           + (1 << 13)) >> 14;
    t12  = (dctint)((t12a + t11a) * 11585U           + (1 << 13)) >> 14;
    t18a = (dctint)(  t29  *  6270U - t18  * 15137U  + (1 << 13)) >> 14;
    t29a = (dctint)(  t29  * 15137U + t18  *  6270U  + (1 << 13)) >> 14;
    t19  = (dctint)(  t28a *  6270U - t19a * 15137U  + (1 << 13)) >> 14;
    t28  = (dctint)(  t28a * 15137U + t19a *  6270U  + (1 << 13)) >> 14;
    t20  = (dctint)(-(t27a * 15137U + t20a *  6270U) + (1 << 13)) >> 14;
    t27  = (dctint)(  t27a *  6270U - t20a * 15137U  + (1 << 13)) >> 14;
    t21a = (dctint)(-(t26  * 15137U + t21  *  6270U) + (1 << 13)) >> 14;
    t26a = (dctint)(  t26  *  6270U - t21  * 15137U  + (1 << 13)) >> 14;

    t0   = t0a + t15a;
    t1   = t1a + t14;
    t2   = t2a + t13a;
    t3   = t3a + t12;
    t4   = t4a + t11;
    t5a  = t5  + t10a;
    t6a  = t6  + t9;
    t7   = t7a + t8a;
    t8   = t7a - t8a;
    t9a  = t6  - t9;
    t10  = t5  - t10a;
    t11a = t4a - t11;
    t12a = t3a - t12;
    t13  = t2a - t13a;
    t14a = t1a - t14;
    t15  = t0a - t15a;
    t16  = t16a + t23a;
    t17a = t17  + t22;
    t18  = t18a + t21a;
    t19a = t19  + t20;
    t20a = t19  - t20;
    t21  = t18a - t21a;
    t22a = t17  - t22;
    t23  = t16a - t23a;
    t24  = t31a - t24a;
    t25a = t30  - t25;
    t26  = t29a - t26a;
    t27a = t28  - t27;
    t28a = t28  + t27;
    t29  = t29a + t26a;
    t30a = t30  + t25;
    t31  = t31a + t24a;

    t20  = (dctint)((t27a - t20a) * 11585U + (1 << 13)) >> 14;
    t27  = (dctint)((t27a + t20a) * 11585U + (1 << 13)) >> 14;
    t21a = (dctint)((t26  - t21 ) * 11585U + (1 << 13)) >> 14;
    t26a = (dctint)((t26  + t21 ) * 11585U + (1 << 13)) >> 14;
    t22  = (dctint)((t25a - t22a) * 11585U + (1 << 13)) >> 14;
    t25  = (dctint)((t25a + t22a) * 11585U + (1 << 13)) >> 14;
    t23a = (dctint)((t24  - t23 ) * 11585U + (1 << 13)) >> 14;
    t24a = (dctint)((t24  + t23 ) * 11585U + (1 << 13)) >> 14;

    out[ 0] = t0   + t31;
    out[ 1] = t1   + t30a;
    out[ 2] = t2   + t29;
    out[ 3] = t3   + t28a;
    out[ 4] = t4   + t27;
    out[ 5] = t5a  + t26a;
    out[ 6] = t6a  + t25;
    out[ 7] = t7   + t24a;
    out[ 8] = t8   + t23a;
    out[ 9] = t9a  + t22;
    out[10] = t10  + t21a;
    out[11] = t11a + t20;
    out[12] = t12a + t19a;
    out[13] = t13  + t18;
    out[14] = t14a + t17a;
    out[15] = t15  + t16;
    out[16] = t15  - t16;
    out[17] = t14a - t17a;
    out[18] = t13  - t18;
    out[19] = t12a - t19a;
    out[20] = t11a - t20;
    out[21] = t10  - t21a;
    out[22] = t9a  - t22;
    out[23] = t8   - t23a;
    out[24] = t7   - t24a;
    out[25] = t6a  - t25;
    out[26] = t5a  - t26a;
    out[27] = t4   - t27;
    out[28] = t3   - t28a;
    out[29] = t2   - t29;
    out[30] = t1   - t30a;
    out[31] = t0   - t31;
}

itxfm_wrapper(idct, idct, 32, 6, 1)

static av_always_inline void iwht4_1d(const dctcoef *in, ptrdiff_t stride,
                                      dctcoef *out, int pass)
{
    int t0, t1, t2, t3, t4;

    if (pass == 0) {
        t0 = IN(0) >> 2;
        t1 = IN(3) >> 2;
        t2 = IN(1) >> 2;
        t3 = IN(2) >> 2;
    } else {
        t0 = IN(0);
        t1 = IN(3);
        t2 = IN(1);
        t3 = IN(2);
    }

    t0 += t2;
    t3 -= t1;
    t4 = (t0 - t3) >> 1;
    t1 = t4 - t1;
    t2 = t4 - t2;
    t0 -= t1;
    t3 += t2;

    out[0] = t0;
    out[1] = t1;
    out[2] = t2;
    out[3] = t3;
}

itxfm_wrapper(iwht, iwht, 4, 0, 0)

#undef IN
#undef itxfm_wrapper
#undef itxfm_wrap

static av_cold void vp9dsp_itxfm_init(VP9DSPContext *dsp)
{
#define init_itxfm(tx, sz) \
    dsp->itxfm_add[tx][DCT_DCT]   = idct_idct_##sz##_add_c; \
    dsp->itxfm_add[tx][DCT_ADST]  = iadst_idct_##sz##_add_c; \
    dsp->itxfm_add[tx][ADST_DCT]  = idct_iadst_##sz##_add_c; \
    dsp->itxfm_add[tx][ADST_ADST] = iadst_iadst_##sz##_add_c

#define init_idct(tx, nm) \
    dsp->itxfm_add[tx][DCT_DCT]   = \
    dsp->itxfm_add[tx][ADST_DCT]  = \
    dsp->itxfm_add[tx][DCT_ADST]  = \
    dsp->itxfm_add[tx][ADST_ADST] = nm##_add_c

    init_itxfm(TX_4X4,   4x4);
    init_itxfm(TX_8X8,   8x8);
    init_itxfm(TX_16X16, 16x16);
    init_idct(TX_32X32,  idct_idct_32x32);
    init_idct(4 /* lossless */, iwht_iwht_4x4);

#undef init_itxfm
#undef init_idct
}

static av_always_inline void loop_filter(pixel *dst, int E, int I, int H,
                                         ptrdiff_t stridea, ptrdiff_t strideb,
                                         int wd)
{
    int i, F = 1 << (BIT_DEPTH - 8);

    E <<= (BIT_DEPTH - 8);
    I <<= (BIT_DEPTH - 8);
    H <<= (BIT_DEPTH - 8);
    for (i = 0; i < 8; i++, dst += stridea) {
        int p7, p6, p5, p4;
        int p3 = dst[strideb * -4], p2 = dst[strideb * -3];
        int p1 = dst[strideb * -2], p0 = dst[strideb * -1];
        int q0 = dst[strideb * +0], q1 = dst[strideb * +1];
        int q2 = dst[strideb * +2], q3 = dst[strideb * +3];
        int q4, q5, q6, q7;
        int fm = FFABS(p3 - p2) <= I && FFABS(p2 - p1) <= I &&
                 FFABS(p1 - p0) <= I && FFABS(q1 - q0) <= I &&
                 FFABS(q2 - q1) <= I && FFABS(q3 - q2) <= I &&
                 FFABS(p0 - q0) * 2 + (FFABS(p1 - q1) >> 1) <= E;
        int flat8out, flat8in;

        if (!fm)
            continue;

        if (wd >= 16) {
            p7 = dst[strideb * -8];
            p6 = dst[strideb * -7];
            p5 = dst[strideb * -6];
            p4 = dst[strideb * -5];
            q4 = dst[strideb * +4];
            q5 = dst[strideb * +5];
            q6 = dst[strideb * +6];
            q7 = dst[strideb * +7];

            flat8out = FFABS(p7 - p0) <= F && FFABS(p6 - p0) <= F &&
                       FFABS(p5 - p0) <= F && FFABS(p4 - p0) <= F &&
                       FFABS(q4 - q0) <= F && FFABS(q5 - q0) <= F &&
                       FFABS(q6 - q0) <= F && FFABS(q7 - q0) <= F;
        }

        if (wd >= 8)
            flat8in = FFABS(p3 - p0) <= F && FFABS(p2 - p0) <= F &&
                      FFABS(p1 - p0) <= F && FFABS(q1 - q0) <= F &&
                      FFABS(q2 - q0) <= F && FFABS(q3 - q0) <= F;

        if (wd >= 16 && flat8out && flat8in) {
            dst[strideb * -7] = (p7 + p7 + p7 + p7 + p7 + p7 + p7 + p6 * 2 +
                                 p5 + p4 + p3 + p2 + p1 + p0 + q0 + 8) >> 4;
            dst[strideb * -6] = (p7 + p7 + p7 + p7 + p7 + p7 + p6 + p5 * 2 +
                                 p4 + p3 + p2 + p1 + p0 + q0 + q1 + 8) >> 4;
            dst[strideb * -5] = (p7 + p7 + p7 + p7 + p7 + p6 + p5 + p4 * 2 +
                                 p3 + p2 + p1 + p0 + q0 + q1 + q2 + 8) >> 4;
            dst[strideb * -4] = (p7 + p7 + p7 + p7 + p6 + p5 + p4 + p3 * 2 +
                                 p2 + p1 + p0 + q0 + q1 + q2 + q3 + 8) >> 4;
            dst[strideb * -3] = (p7 + p7 + p7 + p6 + p5 + p4 + p3 + p2 * 2 +
                                 p1 + p0 + q0 + q1 + q2 + q3 + q4 + 8) >> 4;
            dst[strideb * -2] = (p7 + p7 + p6 + p5 + p4 + p3 + p2 + p1 * 2 +
                                 p0 + q0 + q1 + q2 + q3 + q4 + q5 + 8) >> 4;
            dst[strideb * -1] = (p7 + p6 + p5 + p4 + p3 + p2 + p1 + p0 * 2 +
                                 q0 + q1 + q2 + q3 + q4 + q5 + q6 + 8) >> 4;
            dst[strideb * +0] = (p6 + p5 + p4 + p3 + p2 + p1 + p0 + q0 * 2 +
                                 q1 + q2 + q3 + q4 + q5 + q6 + q7 + 8) >> 4;
            dst[strideb * +1] = (p5 + p4 + p3 + p2 + p1 + p0 + q0 + q1 * 2 +
                                 q2 + q3 + q4 + q5 + q6 + q7 + q7 + 8) >> 4;
            dst[strideb * +2] = (p4 + p3 + p2 + p1 + p0 + q0 + q1 + q2 * 2 +
                                 q3 + q4 + q5 + q6 + q7 + q7 + q7 + 8) >> 4;
            dst[strideb * +3] = (p3 + p2 + p1 + p0 + q0 + q1 + q2 + q3 * 2 +
                                 q4 + q5 + q6 + q7 + q7 + q7 + q7 + 8) >> 4;
            dst[strideb * +4] = (p2 + p1 + p0 + q0 + q1 + q2 + q3 + q4 * 2 +
                                 q5 + q6 + q7 + q7 + q7 + q7 + q7 + 8) >> 4;
            dst[strideb * +5] = (p1 + p0 + q0 + q1 + q2 + q3 + q4 + q5 * 2 +
                                 q6 + q7 + q7 + q7 + q7 + q7 + q7 + 8) >> 4;
            dst[strideb * +6] = (p0 + q0 + q1 + q2 + q3 + q4 + q5 + q6 * 2 +
                                 q7 + q7 + q7 + q7 + q7 + q7 + q7 + 8) >> 4;
        } else if (wd >= 8 && flat8in) {
            dst[strideb * -3] = (p3 + p3 + p3 + 2 * p2 + p1 + p0 + q0 + 4) >> 3;
            dst[strideb * -2] = (p3 + p3 + p2 + 2 * p1 + p0 + q0 + q1 + 4) >> 3;
            dst[strideb * -1] = (p3 + p2 + p1 + 2 * p0 + q0 + q1 + q2 + 4) >> 3;
            dst[strideb * +0] = (p2 + p1 + p0 + 2 * q0 + q1 + q2 + q3 + 4) >> 3;
            dst[strideb * +1] = (p1 + p0 + q0 + 2 * q1 + q2 + q3 + q3 + 4) >> 3;
            dst[strideb * +2] = (p0 + q0 + q1 + 2 * q2 + q3 + q3 + q3 + 4) >> 3;
        } else {
            int hev = FFABS(p1 - p0) > H || FFABS(q1 - q0) > H;

            if (hev) {
                int f = av_clip_intp2(p1 - q1, BIT_DEPTH - 1), f1, f2;
                f = av_clip_intp2(3 * (q0 - p0) + f, BIT_DEPTH - 1);

                f1 = FFMIN(f + 4, (1 << (BIT_DEPTH - 1)) - 1) >> 3;
                f2 = FFMIN(f + 3, (1 << (BIT_DEPTH - 1)) - 1) >> 3;

                dst[strideb * -1] = av_clip_pixel(p0 + f2);
                dst[strideb * +0] = av_clip_pixel(q0 - f1);
            } else {
                int f = av_clip_intp2(3 * (q0 - p0), BIT_DEPTH - 1), f1, f2;

                f1 = FFMIN(f + 4, (1 << (BIT_DEPTH - 1)) - 1) >> 3;
                f2 = FFMIN(f + 3, (1 << (BIT_DEPTH - 1)) - 1) >> 3;

                dst[strideb * -1] = av_clip_pixel(p0 + f2);
                dst[strideb * +0] = av_clip_pixel(q0 - f1);

                f = (f1 + 1) >> 1;
                dst[strideb * -2] = av_clip_pixel(p1 + f);
                dst[strideb * +1] = av_clip_pixel(q1 - f);
            }
        }
    }
}

#define lf_8_fn(dir, wd, stridea, strideb) \
static void loop_filter_##dir##_##wd##_8_c(uint8_t *_dst, \
                                           ptrdiff_t stride, \
                                           int E, int I, int H) \
{ \
    pixel *dst = (pixel *) _dst; \
    stride /= sizeof(pixel); \
    loop_filter(dst, E, I, H, stridea, strideb, wd); \
}

#define lf_8_fns(wd) \
lf_8_fn(h, wd, stride, 1) \
lf_8_fn(v, wd, 1, stride)

lf_8_fns(4)
lf_8_fns(8)
lf_8_fns(16)

#undef lf_8_fn
#undef lf_8_fns

#define lf_16_fn(dir, stridea) \
static void loop_filter_##dir##_16_16_c(uint8_t *dst, \
                                        ptrdiff_t stride, \
                                        int E, int I, int H) \
{ \
    loop_filter_##dir##_16_8_c(dst, stride, E, I, H); \
    loop_filter_##dir##_16_8_c(dst + 8 * stridea, stride, E, I, H); \
}

lf_16_fn(h, stride)
lf_16_fn(v, sizeof(pixel))

#undef lf_16_fn

#define lf_mix_fn(dir, wd1, wd2, stridea) \
static void loop_filter_##dir##_##wd1##wd2##_16_c(uint8_t *dst, \
                                                  ptrdiff_t stride, \
                                                  int E, int I, int H) \
{ \
    loop_filter_##dir##_##wd1##_8_c(dst, stride, E & 0xff, I & 0xff, H & 0xff); \
    loop_filter_##dir##_##wd2##_8_c(dst + 8 * stridea, stride, E >> 8, I >> 8, H >> 8); \
}

#define lf_mix_fns(wd1, wd2) \
lf_mix_fn(h, wd1, wd2, stride) \
lf_mix_fn(v, wd1, wd2, sizeof(pixel))

lf_mix_fns(4, 4)
lf_mix_fns(4, 8)
lf_mix_fns(8, 4)
lf_mix_fns(8, 8)

#undef lf_mix_fn
#undef lf_mix_fns

static av_cold void vp9dsp_loopfilter_init(VP9DSPContext *dsp)
{
    dsp->loop_filter_8[0][0] = loop_filter_h_4_8_c;
    dsp->loop_filter_8[0][1] = loop_filter_v_4_8_c;
    dsp->loop_filter_8[1][0] = loop_filter_h_8_8_c;
    dsp->loop_filter_8[1][1] = loop_filter_v_8_8_c;
    dsp->loop_filter_8[2][0] = loop_filter_h_16_8_c;
    dsp->loop_filter_8[2][1] = loop_filter_v_16_8_c;

    dsp->loop_filter_16[0] = loop_filter_h_16_16_c;
    dsp->loop_filter_16[1] = loop_filter_v_16_16_c;

    dsp->loop_filter_mix2[0][0][0] = loop_filter_h_44_16_c;
    dsp->loop_filter_mix2[0][0][1] = loop_filter_v_44_16_c;
    dsp->loop_filter_mix2[0][1][0] = loop_filter_h_48_16_c;
    dsp->loop_filter_mix2[0][1][1] = loop_filter_v_48_16_c;
    dsp->loop_filter_mix2[1][0][0] = loop_filter_h_84_16_c;
    dsp->loop_filter_mix2[1][0][1] = loop_filter_v_84_16_c;
    dsp->loop_filter_mix2[1][1][0] = loop_filter_h_88_16_c;
    dsp->loop_filter_mix2[1][1][1] = loop_filter_v_88_16_c;
}

#if BIT_DEPTH != 12

static av_always_inline void copy_c(uint8_t *restrict dst, ptrdiff_t dst_stride,
                                    const uint8_t *restrict src,
                                    ptrdiff_t src_stride, int w, int h)
{
    do {
        memcpy(dst, src, w * sizeof(pixel));

        dst += dst_stride;
        src += src_stride;
    } while (--h);
}

static av_always_inline void avg_c(uint8_t *restrict _dst, ptrdiff_t dst_stride,
                                   const uint8_t *restrict _src,
                                   ptrdiff_t src_stride, int w, int h)
{
    pixel *dst = (pixel *) _dst;
    const pixel *src = (const pixel *) _src;

    dst_stride /= sizeof(pixel);
    src_stride /= sizeof(pixel);
    do {
        int x;

        for (x = 0; x < w; x += 4)
            AV_WN4PA(&dst[x], rnd_avg_pixel4(AV_RN4PA(&dst[x]), AV_RN4P(&src[x])));

        dst += dst_stride;
        src += src_stride;
    } while (--h);
}

#define fpel_fn(type, sz) \
static void type##sz##_c(uint8_t *dst, ptrdiff_t dst_stride, \
                         const uint8_t *src, ptrdiff_t src_stride, \
                         int h, int mx, int my) \
{ \
    type##_c(dst, dst_stride, src, src_stride, sz, h); \
}

#define copy_avg_fn(sz) \
fpel_fn(copy, sz) \
fpel_fn(avg,  sz)

copy_avg_fn(64)
copy_avg_fn(32)
copy_avg_fn(16)
copy_avg_fn(8)
copy_avg_fn(4)

#undef fpel_fn
#undef copy_avg_fn

#endif /* BIT_DEPTH != 12 */

#define FILTER_8TAP(src, x, F, stride) \
    av_clip_pixel((F[0] * src[x + -3 * stride] + \
                   F[1] * src[x + -2 * stride] + \
                   F[2] * src[x + -1 * stride] + \
                   F[3] * src[x + +0 * stride] + \
                   F[4] * src[x + +1 * stride] + \
                   F[5] * src[x + +2 * stride] + \
                   F[6] * src[x + +3 * stride] + \
                   F[7] * src[x + +4 * stride] + 64) >> 7)

static av_always_inline void do_8tap_1d_c(uint8_t *_dst, ptrdiff_t dst_stride,
                                          const uint8_t *_src, ptrdiff_t src_stride,
                                          int w, int h, ptrdiff_t ds,
                                          const int16_t *filter, int avg)
{
    pixel *dst = (pixel *) _dst;
    const pixel *src = (const pixel *) _src;

    dst_stride /= sizeof(pixel);
    src_stride /= sizeof(pixel);
    do {
        int x;

        for (x = 0; x < w; x++)
            if (avg) {
                dst[x] = (dst[x] + FILTER_8TAP(src, x, filter, ds) + 1) >> 1;
            } else {
                dst[x] = FILTER_8TAP(src, x, filter, ds);
            }

        dst += dst_stride;
        src += src_stride;
    } while (--h);
}

#define filter_8tap_1d_fn(opn, opa, dir, ds) \
static av_noinline void opn##_8tap_1d_##dir##_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                                const uint8_t *src, ptrdiff_t src_stride, \
                                                int w, int h, const int16_t *filter) \
{ \
    do_8tap_1d_c(dst, dst_stride, src, src_stride, w, h, ds, filter, opa); \
}

filter_8tap_1d_fn(put, 0, v, src_stride / sizeof(pixel))
filter_8tap_1d_fn(put, 0, h, 1)
filter_8tap_1d_fn(avg, 1, v, src_stride / sizeof(pixel))
filter_8tap_1d_fn(avg, 1, h, 1)

#undef filter_8tap_1d_fn

static av_always_inline void do_8tap_2d_c(uint8_t *_dst, ptrdiff_t dst_stride,
                                          const uint8_t *_src, ptrdiff_t src_stride,
                                          int w, int h, const int16_t *filterx,
                                          const int16_t *filtery, int avg)
{
    int tmp_h = h + 7;
    pixel tmp[64 * 71], *tmp_ptr = tmp;
    pixel *dst = (pixel *) _dst;
    const pixel *src = (const pixel *) _src;

    dst_stride /= sizeof(pixel);
    src_stride /= sizeof(pixel);
    src -= src_stride * 3;
    do {
        int x;

        for (x = 0; x < w; x++)
            tmp_ptr[x] = FILTER_8TAP(src, x, filterx, 1);

        tmp_ptr += 64;
        src += src_stride;
    } while (--tmp_h);

    tmp_ptr = tmp + 64 * 3;
    do {
        int x;

        for (x = 0; x < w; x++)
            if (avg) {
                dst[x] = (dst[x] + FILTER_8TAP(tmp_ptr, x, filtery, 64) + 1) >> 1;
            } else {
                dst[x] = FILTER_8TAP(tmp_ptr, x, filtery, 64);
            }

        tmp_ptr += 64;
        dst += dst_stride;
    } while (--h);
}

#define filter_8tap_2d_fn(opn, opa) \
static av_noinline void opn##_8tap_2d_hv_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                           const uint8_t *src, ptrdiff_t src_stride, \
                                           int w, int h, const int16_t *filterx, \
                                           const int16_t *filtery) \
{ \
    do_8tap_2d_c(dst, dst_stride, src, src_stride, w, h, filterx, filtery, opa); \
}

filter_8tap_2d_fn(put, 0)
filter_8tap_2d_fn(avg, 1)

#undef filter_8tap_2d_fn

#define filter_fn_1d(sz, dir, dir_m, type, type_idx, avg) \
static void avg##_8tap_##type##_##sz##dir##_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                              const uint8_t *src, ptrdiff_t src_stride, \
                                              int h, int mx, int my) \
{ \
    avg##_8tap_1d_##dir##_c(dst, dst_stride, src, src_stride, sz, h, \
                            ff_vp9_subpel_filters[type_idx][dir_m]); \
}

#define filter_fn_2d(sz, type, type_idx, avg) \
static void avg##_8tap_##type##_##sz##hv_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                           const uint8_t *src, ptrdiff_t src_stride, \
                                           int h, int mx, int my) \
{ \
    avg##_8tap_2d_hv_c(dst, dst_stride, src, src_stride, sz, h, \
                       ff_vp9_subpel_filters[type_idx][mx], \
                       ff_vp9_subpel_filters[type_idx][my]); \
}

#if BIT_DEPTH != 12

#define FILTER_BILIN(src, x, mxy, stride) \
    (src[x] + ((mxy * (src[x + stride] - src[x]) + 8) >> 4))

static av_always_inline void do_bilin_1d_c(uint8_t *_dst, ptrdiff_t dst_stride,
                                           const uint8_t *_src, ptrdiff_t src_stride,
                                           int w, int h, ptrdiff_t ds, int mxy, int avg)
{
    pixel *dst = (pixel *) _dst;
    const pixel *src = (const pixel *) _src;

    dst_stride /= sizeof(pixel);
    src_stride /= sizeof(pixel);
    do {
        int x;

        for (x = 0; x < w; x++)
            if (avg) {
                dst[x] = (dst[x] + FILTER_BILIN(src, x, mxy, ds) + 1) >> 1;
            } else {
                dst[x] = FILTER_BILIN(src, x, mxy, ds);
            }

        dst += dst_stride;
        src += src_stride;
    } while (--h);
}

#define bilin_1d_fn(opn, opa, dir, ds) \
static av_noinline void opn##_bilin_1d_##dir##_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                                 const uint8_t *src, ptrdiff_t src_stride, \
                                                 int w, int h, int mxy) \
{ \
    do_bilin_1d_c(dst, dst_stride, src, src_stride, w, h, ds, mxy, opa); \
}

bilin_1d_fn(put, 0, v, src_stride / sizeof(pixel))
bilin_1d_fn(put, 0, h, 1)
bilin_1d_fn(avg, 1, v, src_stride / sizeof(pixel))
bilin_1d_fn(avg, 1, h, 1)

#undef bilin_1d_fn

static av_always_inline void do_bilin_2d_c(uint8_t *_dst, ptrdiff_t dst_stride,
                                           const uint8_t *_src, ptrdiff_t src_stride,
                                           int w, int h, int mx, int my, int avg)
{
    pixel tmp[64 * 65], *tmp_ptr = tmp;
    int tmp_h = h + 1;
    pixel *dst = (pixel *) _dst;
    const pixel *src = (const pixel *) _src;

    dst_stride /= sizeof(pixel);
    src_stride /= sizeof(pixel);
    do {
        int x;

        for (x = 0; x < w; x++)
            tmp_ptr[x] = FILTER_BILIN(src, x, mx, 1);

        tmp_ptr += 64;
        src += src_stride;
    } while (--tmp_h);

    tmp_ptr = tmp;
    do {
        int x;

        for (x = 0; x < w; x++)
            if (avg) {
                dst[x] = (dst[x] + FILTER_BILIN(tmp_ptr, x, my, 64) + 1) >> 1;
            } else {
                dst[x] = FILTER_BILIN(tmp_ptr, x, my, 64);
            }

        tmp_ptr += 64;
        dst += dst_stride;
    } while (--h);
}

#define bilin_2d_fn(opn, opa) \
static av_noinline void opn##_bilin_2d_hv_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                            const uint8_t *src, ptrdiff_t src_stride, \
                                            int w, int h, int mx, int my) \
{ \
    do_bilin_2d_c(dst, dst_stride, src, src_stride, w, h, mx, my, opa); \
}

bilin_2d_fn(put, 0)
bilin_2d_fn(avg, 1)

#undef bilin_2d_fn

#define bilinf_fn_1d(sz, dir, dir_m, avg) \
static void avg##_bilin_##sz##dir##_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                      const uint8_t *src, ptrdiff_t src_stride, \
                                      int h, int mx, int my) \
{ \
    avg##_bilin_1d_##dir##_c(dst, dst_stride, src, src_stride, sz, h, dir_m); \
}

#define bilinf_fn_2d(sz, avg) \
static void avg##_bilin_##sz##hv_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                   const uint8_t *src, ptrdiff_t src_stride, \
                                   int h, int mx, int my) \
{ \
    avg##_bilin_2d_hv_c(dst, dst_stride, src, src_stride, sz, h, mx, my); \
}

#else

#define bilinf_fn_1d(a, b, c, d)
#define bilinf_fn_2d(a, b)

#endif

#define filter_fn(sz, avg) \
filter_fn_1d(sz, h, mx, regular, FILTER_8TAP_REGULAR, avg) \
filter_fn_1d(sz, v, my, regular, FILTER_8TAP_REGULAR, avg) \
filter_fn_2d(sz,        regular, FILTER_8TAP_REGULAR, avg) \
filter_fn_1d(sz, h, mx, smooth,  FILTER_8TAP_SMOOTH,  avg) \
filter_fn_1d(sz, v, my, smooth,  FILTER_8TAP_SMOOTH,  avg) \
filter_fn_2d(sz,        smooth,  FILTER_8TAP_SMOOTH,  avg) \
filter_fn_1d(sz, h, mx, sharp,   FILTER_8TAP_SHARP,   avg) \
filter_fn_1d(sz, v, my, sharp,   FILTER_8TAP_SHARP,   avg) \
filter_fn_2d(sz,        sharp,   FILTER_8TAP_SHARP,   avg) \
bilinf_fn_1d(sz, h, mx,                               avg) \
bilinf_fn_1d(sz, v, my,                               avg) \
bilinf_fn_2d(sz,                                      avg)

#define filter_fn_set(avg) \
filter_fn(64, avg) \
filter_fn(32, avg) \
filter_fn(16, avg) \
filter_fn(8,  avg) \
filter_fn(4,  avg)

filter_fn_set(put)
filter_fn_set(avg)

#undef filter_fn
#undef filter_fn_set
#undef filter_fn_1d
#undef filter_fn_2d
#undef bilinf_fn_1d
#undef bilinf_fn_2d

#if BIT_DEPTH != 8
void ff_vp9dsp_mc_init_10(VP9DSPContext *dsp);
#endif
#if BIT_DEPTH != 10
static
#endif
av_cold void FUNC(ff_vp9dsp_mc_init)(VP9DSPContext *dsp)
{
#if BIT_DEPTH == 12
    ff_vp9dsp_mc_init_10(dsp);
#else /* BIT_DEPTH == 12 */

#define init_fpel(idx1, idx2, sz, type) \
    dsp->mc[idx1][FILTER_8TAP_SMOOTH ][idx2][0][0] = type##sz##_c; \
    dsp->mc[idx1][FILTER_8TAP_REGULAR][idx2][0][0] = type##sz##_c; \
    dsp->mc[idx1][FILTER_8TAP_SHARP  ][idx2][0][0] = type##sz##_c; \
    dsp->mc[idx1][FILTER_BILINEAR    ][idx2][0][0] = type##sz##_c

#define init_copy_avg(idx, sz) \
    init_fpel(idx, 0, sz, copy); \
    init_fpel(idx, 1, sz, avg)

    init_copy_avg(0, 64);
    init_copy_avg(1, 32);
    init_copy_avg(2, 16);
    init_copy_avg(3,  8);
    init_copy_avg(4,  4);

#undef init_copy_avg
#undef init_fpel

#endif /* BIT_DEPTH == 12 */

#define init_subpel1_bd_aware(idx1, idx2, idxh, idxv, sz, dir, type) \
    dsp->mc[idx1][FILTER_8TAP_SMOOTH ][idx2][idxh][idxv] = type##_8tap_smooth_##sz##dir##_c; \
    dsp->mc[idx1][FILTER_8TAP_REGULAR][idx2][idxh][idxv] = type##_8tap_regular_##sz##dir##_c; \
    dsp->mc[idx1][FILTER_8TAP_SHARP  ][idx2][idxh][idxv] = type##_8tap_sharp_##sz##dir##_c

#if BIT_DEPTH == 12
#define init_subpel1 init_subpel1_bd_aware
#else
#define init_subpel1(idx1, idx2, idxh, idxv, sz, dir, type) \
    init_subpel1_bd_aware(idx1, idx2, idxh, idxv, sz, dir, type); \
    dsp->mc[idx1][FILTER_BILINEAR    ][idx2][idxh][idxv] = type##_bilin_##sz##dir##_c
#endif

#define init_subpel2(idx, idxh, idxv, dir, type) \
    init_subpel1(0, idx, idxh, idxv, 64, dir, type); \
    init_subpel1(1, idx, idxh, idxv, 32, dir, type); \
    init_subpel1(2, idx, idxh, idxv, 16, dir, type); \
    init_subpel1(3, idx, idxh, idxv,  8, dir, type); \
    init_subpel1(4, idx, idxh, idxv,  4, dir, type)

#define init_subpel3(idx, type) \
    init_subpel2(idx, 1, 1, hv, type); \
    init_subpel2(idx, 0, 1, v, type); \
    init_subpel2(idx, 1, 0, h, type)

    init_subpel3(0, put);
    init_subpel3(1, avg);

#undef init_subpel1
#undef init_subpel2
#undef init_subpel3
#undef init_subpel1_bd_aware
}

static av_always_inline void do_scaled_8tap_c(uint8_t *_dst, ptrdiff_t dst_stride,
                                              const uint8_t *_src, ptrdiff_t src_stride,
                                              int w, int h, int mx, int my,
                                              int dx, int dy, int avg,
                                              const int16_t (*filters)[8])
{
    int tmp_h = (((h - 1) * dy + my) >> 4) + 8;
    pixel tmp[64 * 135], *tmp_ptr = tmp;
    pixel *dst = (pixel *) _dst;
    const pixel *src = (const pixel *) _src;

    dst_stride /= sizeof(pixel);
    src_stride /= sizeof(pixel);
    src -= src_stride * 3;
    do {
        int x;
        int imx = mx, ioff = 0;

        for (x = 0; x < w; x++) {
            tmp_ptr[x] = FILTER_8TAP(src, ioff, filters[imx], 1);
            imx += dx;
            ioff += imx >> 4;
            imx &= 0xf;
        }

        tmp_ptr += 64;
        src += src_stride;
    } while (--tmp_h);

    tmp_ptr = tmp + 64 * 3;
    do {
        int x;
        const int16_t *filter = filters[my];

        for (x = 0; x < w; x++)
            if (avg) {
                dst[x] = (dst[x] + FILTER_8TAP(tmp_ptr, x, filter, 64) + 1) >> 1;
            } else {
                dst[x] = FILTER_8TAP(tmp_ptr, x, filter, 64);
            }

        my += dy;
        tmp_ptr += (my >> 4) * 64;
        my &= 0xf;
        dst += dst_stride;
    } while (--h);
}

#define scaled_filter_8tap_fn(opn, opa) \
static av_noinline void opn##_scaled_8tap_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                            const uint8_t *src, ptrdiff_t src_stride, \
                                            int w, int h, int mx, int my, int dx, int dy, \
                                            const int16_t (*filters)[8]) \
{ \
    do_scaled_8tap_c(dst, dst_stride, src, src_stride, w, h, mx, my, dx, dy, \
                     opa, filters); \
}

scaled_filter_8tap_fn(put, 0)
scaled_filter_8tap_fn(avg, 1)

#undef scaled_filter_8tap_fn

#undef FILTER_8TAP

#define scaled_filter_fn(sz, type, type_idx, avg) \
static void avg##_scaled_##type##_##sz##_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                           const uint8_t *src, ptrdiff_t src_stride, \
                                           int h, int mx, int my, int dx, int dy) \
{ \
    avg##_scaled_8tap_c(dst, dst_stride, src, src_stride, sz, h, mx, my, dx, dy, \
                        ff_vp9_subpel_filters[type_idx]); \
}

#if BIT_DEPTH != 12

static av_always_inline void do_scaled_bilin_c(uint8_t *_dst, ptrdiff_t dst_stride,
                                               const uint8_t *_src, ptrdiff_t src_stride,
                                               int w, int h, int mx, int my,
                                               int dx, int dy, int avg)
{
    pixel tmp[64 * 129], *tmp_ptr = tmp;
    int tmp_h = (((h - 1) * dy + my) >> 4) + 2;
    pixel *dst = (pixel *) _dst;
    const pixel *src = (const pixel *) _src;

    dst_stride /= sizeof(pixel);
    src_stride /= sizeof(pixel);
    do {
        int x;
        int imx = mx, ioff = 0;

        for (x = 0; x < w; x++) {
            tmp_ptr[x] = FILTER_BILIN(src, ioff, imx, 1);
            imx += dx;
            ioff += imx >> 4;
            imx &= 0xf;
        }

        tmp_ptr += 64;
        src += src_stride;
    } while (--tmp_h);

    tmp_ptr = tmp;
    do {
        int x;

        for (x = 0; x < w; x++)
            if (avg) {
                dst[x] = (dst[x] + FILTER_BILIN(tmp_ptr, x, my, 64) + 1) >> 1;
            } else {
                dst[x] = FILTER_BILIN(tmp_ptr, x, my, 64);
            }

        my += dy;
        tmp_ptr += (my >> 4) * 64;
        my &= 0xf;
        dst += dst_stride;
    } while (--h);
}

#define scaled_bilin_fn(opn, opa) \
static av_noinline void opn##_scaled_bilin_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                             const uint8_t *src, ptrdiff_t src_stride, \
                                             int w, int h, int mx, int my, int dx, int dy) \
{ \
    do_scaled_bilin_c(dst, dst_stride, src, src_stride, w, h, mx, my, dx, dy, opa); \
}

scaled_bilin_fn(put, 0)
scaled_bilin_fn(avg, 1)

#undef scaled_bilin_fn

#undef FILTER_BILIN

#define scaled_bilinf_fn(sz, avg) \
static void avg##_scaled_bilin_##sz##_c(uint8_t *dst, ptrdiff_t dst_stride, \
                                        const uint8_t *src, ptrdiff_t src_stride, \
                                        int h, int mx, int my, int dx, int dy) \
{ \
    avg##_scaled_bilin_c(dst, dst_stride, src, src_stride, sz, h, mx, my, dx, dy); \
}

#else

#define scaled_bilinf_fn(a, b)

#endif

#define scaled_filter_fns(sz, avg) \
scaled_filter_fn(sz,        regular, FILTER_8TAP_REGULAR, avg) \
scaled_filter_fn(sz,        smooth,  FILTER_8TAP_SMOOTH,  avg) \
scaled_filter_fn(sz,        sharp,   FILTER_8TAP_SHARP,   avg) \
scaled_bilinf_fn(sz,                                      avg)

#define scaled_filter_fn_set(avg) \
scaled_filter_fns(64, avg) \
scaled_filter_fns(32, avg) \
scaled_filter_fns(16, avg) \
scaled_filter_fns(8,  avg) \
scaled_filter_fns(4,  avg)

scaled_filter_fn_set(put)
scaled_filter_fn_set(avg)

#undef scaled_filter_fns
#undef scaled_filter_fn_set
#undef scaled_filter_fn
#undef scaled_bilinf_fn

#if BIT_DEPTH != 8
void ff_vp9dsp_scaled_mc_init_10(VP9DSPContext *dsp);
#endif
#if BIT_DEPTH != 10
static
#endif
av_cold void FUNC(ff_vp9dsp_scaled_mc_init)(VP9DSPContext *dsp)
{
#define init_scaled_bd_aware(idx1, idx2, sz, type) \
    dsp->smc[idx1][FILTER_8TAP_SMOOTH ][idx2] = type##_scaled_smooth_##sz##_c; \
    dsp->smc[idx1][FILTER_8TAP_REGULAR][idx2] = type##_scaled_regular_##sz##_c; \
    dsp->smc[idx1][FILTER_8TAP_SHARP  ][idx2] = type##_scaled_sharp_##sz##_c

#if BIT_DEPTH == 12
    ff_vp9dsp_scaled_mc_init_10(dsp);
#define init_scaled(a,b,c,d) init_scaled_bd_aware(a,b,c,d)
#else
#define init_scaled(idx1, idx2, sz, type) \
    init_scaled_bd_aware(idx1, idx2, sz, type); \
    dsp->smc[idx1][FILTER_BILINEAR    ][idx2] = type##_scaled_bilin_##sz##_c
#endif

#define init_scaled_put_avg(idx, sz) \
    init_scaled(idx, 0, sz, put); \
    init_scaled(idx, 1, sz, avg)

    init_scaled_put_avg(0, 64);
    init_scaled_put_avg(1, 32);
    init_scaled_put_avg(2, 16);
    init_scaled_put_avg(3,  8);
    init_scaled_put_avg(4,  4);

#undef init_scaled_put_avg
#undef init_scaled
#undef init_scaled_bd_aware
}

av_cold void FUNC(ff_vp9dsp_init)(VP9DSPContext *dsp)
{
    FUNC(ff_vp9dsp_intrapred_init)(dsp);
    vp9dsp_itxfm_init(dsp);
    vp9dsp_loopfilter_init(dsp);
    FUNC(ff_vp9dsp_mc_init)(dsp);
    FUNC(ff_vp9dsp_scaled_mc_init)(dsp);
}
