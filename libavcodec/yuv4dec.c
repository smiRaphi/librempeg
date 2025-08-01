/*
 * libquicktime yuv4 decoder
 *
 * Copyright (c) 2011 Carl Eugen Hoyos
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

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

static av_cold int yuv4_decode_init(AVCodecContext *avctx)
{
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    return 0;
}

static int yuv4_decode_frame(AVCodecContext *avctx, AVFrame *pic,
                             int *got_frame, AVPacket *avpkt)
{
    const uint8_t *src = avpkt->data;
    uint8_t *y, *u, *v;
    int i, j, ret;

    if (avpkt->size < 6 * (avctx->width + 1 >> 1) * (avctx->height + 1 >> 1)) {
        av_log(avctx, AV_LOG_ERROR, "Insufficient input data.\n");
        return AVERROR(EINVAL);
    }

    if ((ret = ff_get_buffer(avctx, pic, 0)) < 0)
        return ret;

    y = pic->data[0];
    u = pic->data[1];
    v = pic->data[2];

    for (i = 0; i < (avctx->height + 1) >> 1; i++) {
        for (j = 0; j < (avctx->width + 1) >> 1; j++) {
            u[j] = *src++ ^ 0x80;
            v[j] = *src++ ^ 0x80;
            y[                   2 * j    ] = *src++;
            y[                   2 * j + 1] = *src++;
            y[pic->linesize[0] + 2 * j    ] = *src++;
            y[pic->linesize[0] + 2 * j + 1] = *src++;
        }

        y += 2 * pic->linesize[0];
        u +=     pic->linesize[1];
        v +=     pic->linesize[2];
    }

    *got_frame = 1;

    return avpkt->size;
}

const FFCodec ff_yuv4_decoder = {
    .p.name       = "yuv4",
    CODEC_LONG_NAME("Uncompressed packed 4:2:0"),
    .p.type       = AVMEDIA_TYPE_VIDEO,
    .p.id         = AV_CODEC_ID_YUV4,
    .init         = yuv4_decode_init,
    FF_CODEC_DECODE_CB(yuv4_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
