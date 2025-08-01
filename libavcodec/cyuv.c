/*
 * Creative YUV (CYUV) Video Decoder
 *   by Mike Melanson (melanson@pcisys.net)
 * based on "Creative YUV (CYUV) stream format for AVI":
 *   http://www.csse.monash.edu.au/~timf/videocodec/cyuv.txt
 *
 * Copyright (C) 2003 The FFmpeg project
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

/**
 * @file
 * Creative YUV (CYUV) Video Decoder.
 */

#include "config_components.h"

#include <string.h>

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "libavutil/internal.h"

static av_cold int cyuv_decode_init(AVCodecContext *avctx)
{
    /* width needs to be divisible by 4 for this codec to work */
    if (avctx->width & 0x3)
        return AVERROR_INVALIDDATA;

    return 0;
}

static int cyuv_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;

    unsigned char *y_plane;
    unsigned char *u_plane;
    unsigned char *v_plane;
    int y_ptr;
    int u_ptr;
    int v_ptr;

    /* prediction error tables (make it clear that they are signed values) */
    const signed char *y_table = (const signed char*)buf +  0;
    const signed char *u_table = (const signed char*)buf + 16;
    const signed char *v_table = (const signed char*)buf + 32;

    unsigned char y_pred, u_pred, v_pred;
    int stream_ptr;
    unsigned char cur_byte;
    int pixel_groups;
    int rawsize = avctx->height * FFALIGN(avctx->width,2) * 2;
    int ret;

    if (avctx->codec_id == AV_CODEC_ID_AURA) {
        y_table = u_table;
        u_table = v_table;
    }
    /* sanity check the buffer size: A buffer has 3x16-bytes tables
     * followed by (height) lines each with 3 bytes to represent groups
     * of 4 pixels. Thus, the total size of the buffer ought to be:
     *    (3 * 16) + height * (width * 3 / 4) */
    if (buf_size == 48 + avctx->height * (avctx->width * 3 / 4)) {
        avctx->pix_fmt = AV_PIX_FMT_YUV411P;
    } else if(buf_size == rawsize ) {
        avctx->pix_fmt = AV_PIX_FMT_UYVY422;
    } else {
        av_log(avctx, AV_LOG_ERROR, "got a buffer with %d bytes when %d were expected\n",
               buf_size, 48 + avctx->height * (avctx->width * 3 / 4));
        return AVERROR_INVALIDDATA;
    }

    /* pixel data starts 48 bytes in, after 3x16-byte tables */
    stream_ptr = 48;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    y_plane = frame->data[0];
    u_plane = frame->data[1];
    v_plane = frame->data[2];

    if (buf_size == rawsize) {
        int linesize = FFALIGN(avctx->width, 2) * 2;
        y_plane += frame->linesize[0] * avctx->height;
        for (stream_ptr = 0; stream_ptr < rawsize; stream_ptr += linesize) {
            y_plane -= frame->linesize[0];
            memcpy(y_plane, buf+stream_ptr, linesize);
        }
    } else {

    /* iterate through each line in the height */
    for (y_ptr = 0, u_ptr = 0, v_ptr = 0;
         y_ptr < (avctx->height * frame->linesize[0]);
         y_ptr += frame->linesize[0] - avctx->width,
         u_ptr += frame->linesize[1] - avctx->width / 4,
         v_ptr += frame->linesize[2] - avctx->width / 4) {

        /* reset predictors */
        cur_byte = buf[stream_ptr++];
        u_plane[u_ptr++] = u_pred = cur_byte & 0xF0;
        y_plane[y_ptr++] = y_pred = (cur_byte & 0x0F) << 4;

        cur_byte = buf[stream_ptr++];
        v_plane[v_ptr++] = v_pred = cur_byte & 0xF0;
        y_pred += y_table[cur_byte & 0x0F];
        y_plane[y_ptr++] = y_pred;

        cur_byte = buf[stream_ptr++];
        y_pred += y_table[cur_byte & 0x0F];
        y_plane[y_ptr++] = y_pred;
        y_pred += y_table[(cur_byte & 0xF0) >> 4];
        y_plane[y_ptr++] = y_pred;

        /* iterate through the remaining pixel groups (4 pixels/group) */
        pixel_groups = avctx->width / 4 - 1;
        while (pixel_groups--) {

            cur_byte = buf[stream_ptr++];
            u_pred += u_table[(cur_byte & 0xF0) >> 4];
            u_plane[u_ptr++] = u_pred;
            y_pred += y_table[cur_byte & 0x0F];
            y_plane[y_ptr++] = y_pred;

            cur_byte = buf[stream_ptr++];
            v_pred += v_table[(cur_byte & 0xF0) >> 4];
            v_plane[v_ptr++] = v_pred;
            y_pred += y_table[cur_byte & 0x0F];
            y_plane[y_ptr++] = y_pred;

            cur_byte = buf[stream_ptr++];
            y_pred += y_table[cur_byte & 0x0F];
            y_plane[y_ptr++] = y_pred;
            y_pred += y_table[(cur_byte & 0xF0) >> 4];
            y_plane[y_ptr++] = y_pred;

        }
    }
    }

    *got_frame = 1;

    return buf_size;
}

#if CONFIG_AURA_DECODER
const FFCodec ff_aura_decoder = {
    .p.name         = "aura",
    CODEC_LONG_NAME("Auravision AURA"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_AURA,
    .init           = cyuv_decode_init,
    FF_CODEC_DECODE_CB(cyuv_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
#endif

#if CONFIG_CYUV_DECODER
const FFCodec ff_cyuv_decoder = {
    .p.name         = "cyuv",
    CODEC_LONG_NAME("Creative YUV (CYUV)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_CYUV,
    .init           = cyuv_decode_init,
    FF_CODEC_DECODE_CB(cyuv_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
#endif
