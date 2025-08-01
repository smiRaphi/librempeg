/*
 * Texture block compression and decompression
 * Copyright (C) 2015 Vittorio Giovara <vittorio.giovara@gmail.com>
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
 *
 */

#include "avcodec.h"

static int exec_func(AVCodecContext *avctx, void *arg,
                     int slice, int thread_nb)
{
    const TextureDSPThreadContext *ctx = arg;
    uint8_t *d = ctx->tex_data.out;
    int w_block = ctx->width  / TEXTURE_BLOCK_W;
    int h_block = ctx->height / TEXTURE_BLOCK_H;
    int x, y;
    int start_slice, end_slice;
    int base_blocks_per_slice = h_block / ctx->slice_count;
    int remainder_blocks = h_block % ctx->slice_count;

    /* When the frame height (in blocks) doesn't divide evenly between the
     * number of slices, spread the remaining blocks evenly between the first
     * operations */
    start_slice = slice * base_blocks_per_slice;
    /* Add any extra blocks (one per slice) that have been added before this slice */
    start_slice += FFMIN(slice, remainder_blocks);

    end_slice = start_slice + base_blocks_per_slice;
    /* Add an extra block if there are still remainder blocks to be accounted for */
    if (slice < remainder_blocks)
        end_slice++;

    for (y = start_slice; y < end_slice; y++) {
        uint8_t *p = ctx->frame_data.out + y * ctx->stride * TEXTURE_BLOCK_H;
        int off = y * w_block;
        for (x = 0; x < w_block; x++) {
            ctx->TEXTUREDSP_TEX_FUNC(p + x * ctx->raw_ratio, ctx->stride,
                                     d + (off + x) * ctx->tex_ratio);
        }
    }

    return 0;
}

int TEXTUREDSP_FUNC_NAME(AVCodecContext *avctx, TextureDSPThreadContext *ctx)
{
    return avctx->execute2(avctx, exec_func, ctx, NULL, ctx->slice_count);
}
