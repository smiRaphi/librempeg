/*
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
 * id RoQ Video Decoder by Dr. Tim Ferguson
 * For more information about the id RoQ format, visit:
 *   http://www.csse.monash.edu.au/~timf/
 */

#include "libavutil/avassert.h"

#include "avcodec.h"
#include "bytestream.h"
#include "codec_internal.h"
#include "decode.h"
#include "roqvideo.h"

static void roqvideo_decode_frame(RoqContext *ri, GetByteContext *gb)
{
    unsigned int chunk_id = 0, chunk_arg = 0;
    unsigned long chunk_size = 0;
    int i, j, k, nv1, nv2, vqflg = 0, vqflg_pos = -1;
    int vqid, xpos, ypos, xp, yp, x, y, mx, my;
    roq_qcell *qcell;
    int64_t chunk_start;

    while (bytestream2_get_bytes_left(gb) >= 8) {
        chunk_id   = bytestream2_get_le16(gb);
        chunk_size = bytestream2_get_le32(gb);
        chunk_arg  = bytestream2_get_le16(gb);

        if(chunk_id == RoQ_QUAD_VQ)
            break;
        if(chunk_id == RoQ_QUAD_CODEBOOK) {
            if((nv1 = chunk_arg >> 8) == 0)
                nv1 = 256;
            if((nv2 = chunk_arg & 0xff) == 0 && nv1 * 6 < chunk_size)
                nv2 = 256;
            for(i = 0; i < nv1; i++) {
                ri->cb2x2[i].y[0] = bytestream2_get_byte(gb);
                ri->cb2x2[i].y[1] = bytestream2_get_byte(gb);
                ri->cb2x2[i].y[2] = bytestream2_get_byte(gb);
                ri->cb2x2[i].y[3] = bytestream2_get_byte(gb);
                ri->cb2x2[i].u    = bytestream2_get_byte(gb);
                ri->cb2x2[i].v    = bytestream2_get_byte(gb);
            }
            for(i = 0; i < nv2; i++)
                for(j = 0; j < 4; j++)
                    ri->cb4x4[i].idx[j] = bytestream2_get_byte(gb);
        }
    }

    chunk_start = bytestream2_tell(gb);
    xpos = ypos = 0;

    if (chunk_size > bytestream2_get_bytes_left(gb)) {
        av_log(ri->logctx, AV_LOG_ERROR, "Chunk does not fit in input buffer\n");
        chunk_size = bytestream2_get_bytes_left(gb);
    }

    while (bytestream2_tell(gb) < chunk_start + chunk_size) {
        for (yp = ypos; yp < ypos + 16; yp += 8)
            for (xp = xpos; xp < xpos + 16; xp += 8) {
                if (bytestream2_tell(gb) >= chunk_start + chunk_size) {
                    av_log(ri->logctx, AV_LOG_VERBOSE, "Chunk is too short\n");
                    return;
                }
                if (vqflg_pos < 0) {
                    vqflg = bytestream2_get_le16(gb);
                    vqflg_pos = 7;
                }
                vqid = (vqflg >> (vqflg_pos * 2)) & 0x3;
                vqflg_pos--;

                switch(vqid) {
                case RoQ_ID_MOT:
                    break;
                case RoQ_ID_FCC: {
                    int byte = bytestream2_get_byte(gb);
                    mx = 8 - (byte >> 4) - ((signed char) (chunk_arg >> 8));
                    my = 8 - (byte & 0xf) - ((signed char) chunk_arg);
                    ff_apply_motion_8x8(ri, xp, yp, mx, my);
                    break;
                }
                case RoQ_ID_SLD:
                    qcell = ri->cb4x4 + bytestream2_get_byte(gb);
                    ff_apply_vector_4x4(ri, xp,     yp,     ri->cb2x2 + qcell->idx[0]);
                    ff_apply_vector_4x4(ri, xp + 4, yp,     ri->cb2x2 + qcell->idx[1]);
                    ff_apply_vector_4x4(ri, xp,     yp + 4, ri->cb2x2 + qcell->idx[2]);
                    ff_apply_vector_4x4(ri, xp + 4, yp + 4, ri->cb2x2 + qcell->idx[3]);
                    break;
                case RoQ_ID_CCC:
                    for (k = 0; k < 4; k++) {
                        x = xp; y = yp;
                        if(k & 0x01) x += 4;
                        if(k & 0x02) y += 4;

                        if (bytestream2_tell(gb) >= chunk_start + chunk_size) {
                            av_log(ri->logctx, AV_LOG_VERBOSE, "Chunk is too short\n");
                            return;
                        }
                        if (vqflg_pos < 0) {
                            vqflg = bytestream2_get_le16(gb);
                            vqflg_pos = 7;
                        }
                        vqid = (vqflg >> (vqflg_pos * 2)) & 0x3;
                        vqflg_pos--;
                        switch(vqid) {
                        case RoQ_ID_MOT:
                            break;
                        case RoQ_ID_FCC: {
                            int byte = bytestream2_get_byte(gb);
                            mx = 8 - (byte >> 4) - ((signed char) (chunk_arg >> 8));
                            my = 8 - (byte & 0xf) - ((signed char) chunk_arg);
                            ff_apply_motion_4x4(ri, x, y, mx, my);
                            break;
                        }
                        case RoQ_ID_SLD:
                            qcell = ri->cb4x4 + bytestream2_get_byte(gb);
                            ff_apply_vector_2x2(ri, x,     y,     ri->cb2x2 + qcell->idx[0]);
                            ff_apply_vector_2x2(ri, x + 2, y,     ri->cb2x2 + qcell->idx[1]);
                            ff_apply_vector_2x2(ri, x,     y + 2, ri->cb2x2 + qcell->idx[2]);
                            ff_apply_vector_2x2(ri, x + 2, y + 2, ri->cb2x2 + qcell->idx[3]);
                            break;
                        case RoQ_ID_CCC:
                            ff_apply_vector_2x2(ri, x,     y,     ri->cb2x2 + bytestream2_get_byte(gb));
                            ff_apply_vector_2x2(ri, x + 2, y,     ri->cb2x2 + bytestream2_get_byte(gb));
                            ff_apply_vector_2x2(ri, x,     y + 2, ri->cb2x2 + bytestream2_get_byte(gb));
                            ff_apply_vector_2x2(ri, x + 2, y + 2, ri->cb2x2 + bytestream2_get_byte(gb));
                            break;
                        }
                    }
                    break;
                default:
                    av_assert2(0);
            }
        }

        xpos += 16;
        if (xpos >= ri->width) {
            xpos -= ri->width;
            ypos += 16;
        }
        if(ypos >= ri->height)
            break;
    }
}


static av_cold int roq_decode_init(AVCodecContext *avctx)
{
    RoqContext *s = avctx->priv_data;

    s->logctx = avctx;

    if (avctx->width % 16 || avctx->height % 16) {
        avpriv_request_sample(avctx, "Dimensions not being a multiple of 16");
        return AVERROR_PATCHWELCOME;
    }

    s->width = avctx->width;
    s->height = avctx->height;

    s->last_frame    = av_frame_alloc();
    s->current_frame = av_frame_alloc();
    if (!s->current_frame || !s->last_frame)
        return AVERROR(ENOMEM);

    avctx->pix_fmt = AV_PIX_FMT_YUVJ444P;
    avctx->color_range = AVCOL_RANGE_JPEG;

    return 0;
}

static int roq_decode_frame(AVCodecContext *avctx, AVFrame *rframe,
                            int *got_frame, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    RoqContext *s = avctx->priv_data;
    int copy = !s->current_frame->data[0] && s->last_frame->data[0];
    GetByteContext gb;
    int ret;

    if ((ret = ff_reget_buffer(avctx, s->current_frame, 0)) < 0)
        return ret;

    if (copy) {
        ret = av_frame_copy(s->current_frame, s->last_frame);
        if (ret < 0)
            return ret;
    }

    bytestream2_init(&gb, buf, buf_size);
    roqvideo_decode_frame(s, &gb);

    if ((ret = av_frame_ref(rframe, s->current_frame)) < 0)
        return ret;
    *got_frame      = 1;

    /* shuffle frames */
    FFSWAP(AVFrame *, s->current_frame, s->last_frame);

    return buf_size;
}

static av_cold int roq_decode_end(AVCodecContext *avctx)
{
    RoqContext *s = avctx->priv_data;

    av_frame_free(&s->current_frame);
    av_frame_free(&s->last_frame);

    return 0;
}

const FFCodec ff_roq_decoder = {
    .p.name         = "roqvideo",
    CODEC_LONG_NAME("id RoQ video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ROQ,
    .priv_data_size = sizeof(RoqContext),
    .init           = roq_decode_init,
    .close          = roq_decode_end,
    FF_CODEC_DECODE_CB(roq_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
