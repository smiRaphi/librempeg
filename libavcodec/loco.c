/*
 * LOCO codec
 * Copyright (c) 2005 Konstantin Shishkov
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
 * LOCO codec.
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "golomb.h"
#include "mathops.h"

enum LOCO_MODE {
    LOCO_UNKN  =  0,
    LOCO_CYUY2 = -1,
    LOCO_CRGB  = -2,
    LOCO_CRGBA = -3,
    LOCO_CYV12 = -4,
    LOCO_YUY2  =  1,
    LOCO_UYVY  =  2,
    LOCO_RGB   =  3,
    LOCO_RGBA  =  4,
    LOCO_YV12  =  5,
};

typedef struct LOCOContext {
    AVCodecContext *avctx;
    int lossy;
    enum LOCO_MODE mode;
} LOCOContext;

typedef struct RICEContext {
    GetBitContext gb;
    int save, run, run2; /* internal rice decoder state */
    int sum, count; /* sum and count for getting rice parameter */
    int lossy;
} RICEContext;

static int loco_get_rice_param(RICEContext *r)
{
    int cnt = 0;
    int val = r->count;

    while (r->sum > val && cnt < 9) {
        val <<= 1;
        cnt++;
    }

    return cnt;
}

static inline void loco_update_rice_param(RICEContext *r, int val)
{
    r->sum += val;
    r->count++;

    if (r->count == 16) {
        r->sum   >>= 1;
        r->count >>= 1;
    }
}

static inline int loco_get_rice(RICEContext *r)
{
    unsigned v;
    if (r->run > 0) { /* we have zero run */
        r->run--;
        loco_update_rice_param(r, 0);
        return 0;
    }
    if (get_bits_left(&r->gb) < 1)
        return INT_MIN;
    v = get_ur_golomb_jpegls(&r->gb, loco_get_rice_param(r), INT_MAX, 0);
    if (v == -1)
        return INT_MIN;
    loco_update_rice_param(r, (v + 1) >> 1);
    if (!v) {
        if (r->save >= 0) {
            int run = get_ur_golomb_jpegls(&r->gb, 2, INT_MAX, 0);
            if (run == -1)
                return INT_MIN;
            r->run = run;
            if (r->run > 1)
                r->save += r->run + 1;
            else
                r->save -= 3;
        } else
            r->run2++;
    } else {
        v = ((v >> 1) + r->lossy) ^ -(v & 1);
        if (r->run2 > 0) {
            if (r->run2 > 2)
                r->save += r->run2;
            else
                r->save -= 3;
            r->run2 = 0;
        }
    }

    return v;
}

/* LOCO main predictor - LOCO-I/JPEG-LS predictor */
static inline int loco_predict(uint8_t* data, int stride)
{
    int a, b, c;

    a = data[-stride];
    b = data[-1];
    c = data[-stride - 1];

    return mid_pred(a, a + b - c, b);
}

static int loco_decode_plane(LOCOContext *l, uint8_t *data, int width, int height,
                             int stride, const uint8_t *buf, int buf_size)
{
    RICEContext rc;
    unsigned val;
    int ret;
    int i, j;

    if(buf_size<=0)
        return -1;

    if ((ret = init_get_bits8(&rc.gb, buf, buf_size)) < 0)
        return ret;

    rc.save  = 0;
    rc.run   = 0;
    rc.run2  = 0;
    rc.lossy = l->lossy;

    rc.sum   = 8;
    rc.count = 1;

    /* restore top left pixel */
    val     = loco_get_rice(&rc);
    if (val == INT_MIN)
        return AVERROR_INVALIDDATA;
    data[0] = 128 + val;
    /* restore top line */
    for (i = 1; i < width; i++) {
        val = loco_get_rice(&rc);
        if (val == INT_MIN)
           return AVERROR_INVALIDDATA;
        data[i] = data[i - 1] + val;
    }
    data += stride;
    for (j = 1; j < height; j++) {
        /* restore left column */
        val = loco_get_rice(&rc);
        if (val == INT_MIN)
           return AVERROR_INVALIDDATA;
        data[0] = data[-stride] + val;
        /* restore all other pixels */
        for (i = 1; i < width; i++) {
            val = loco_get_rice(&rc);
            if (val == INT_MIN)
                return -1;
            data[i] = loco_predict(&data[i], stride) + val;
        }
        data += stride;
    }

    return (get_bits_count(&rc.gb) + 7) >> 3;
}

static void rotate_faulty_loco(uint8_t *data, int width, int height, int stride)
{
    int y;

    for (y=1; y<height; y++) {
        if (width>=y) {
            memmove(data + y*stride,
                    data + y*(stride + 1),
                    (width-y));
            if (y+1 < height)
                memmove(data + y*stride + (width-y),
                        data + (y+1)*stride, y);
        }
    }
}

static int decode_frame(AVCodecContext *avctx, AVFrame *p,
                        int *got_frame, AVPacket *avpkt)
{
    LOCOContext * const l = avctx->priv_data;
    const uint8_t *buf    = avpkt->data;
    int buf_size          = avpkt->size;
    int decoded, ret;

    if ((ret = ff_get_buffer(avctx, p, 0)) < 0)
        return ret;

#define ADVANCE_BY_DECODED do { \
    if (decoded < 0 || decoded >= buf_size) goto buf_too_small; \
    buf += decoded; buf_size -= decoded; \
} while(0)
    switch(l->mode) {
    case LOCO_CYUY2: case LOCO_YUY2: case LOCO_UYVY:
        decoded = loco_decode_plane(l, p->data[0], avctx->width, avctx->height,
                                    p->linesize[0], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[1], avctx->width / 2, avctx->height,
                                    p->linesize[1], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[2], avctx->width / 2, avctx->height,
                                    p->linesize[2], buf, buf_size);
        break;
    case LOCO_CYV12: case LOCO_YV12:
        decoded = loco_decode_plane(l, p->data[0], avctx->width, avctx->height,
                                    p->linesize[0], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[2], avctx->width / 2, avctx->height / 2,
                                    p->linesize[2], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[1], avctx->width / 2, avctx->height / 2,
                                    p->linesize[1], buf, buf_size);
        break;
    case LOCO_CRGB: case LOCO_RGB:
        decoded = loco_decode_plane(l, p->data[1] + p->linesize[1]*(avctx->height-1), avctx->width, avctx->height,
                                    -p->linesize[1], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[0] + p->linesize[0]*(avctx->height-1), avctx->width, avctx->height,
                                    -p->linesize[0], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[2] + p->linesize[2]*(avctx->height-1), avctx->width, avctx->height,
                                    -p->linesize[2], buf, buf_size);
        if (avctx->width & 1) {
            rotate_faulty_loco(p->data[0] + p->linesize[0]*(avctx->height-1), avctx->width, avctx->height, -p->linesize[0]);
            rotate_faulty_loco(p->data[1] + p->linesize[1]*(avctx->height-1), avctx->width, avctx->height, -p->linesize[1]);
            rotate_faulty_loco(p->data[2] + p->linesize[2]*(avctx->height-1), avctx->width, avctx->height, -p->linesize[2]);
        }
        break;
    case LOCO_CRGBA:
    case LOCO_RGBA:
        decoded = loco_decode_plane(l, p->data[1] + p->linesize[1]*(avctx->height-1), avctx->width, avctx->height,
                                    -p->linesize[1], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[0] + p->linesize[0]*(avctx->height-1), avctx->width, avctx->height,
                                    -p->linesize[0], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[2] + p->linesize[2]*(avctx->height-1), avctx->width, avctx->height,
                                    -p->linesize[2], buf, buf_size);
        ADVANCE_BY_DECODED;
        decoded = loco_decode_plane(l, p->data[3] + p->linesize[3]*(avctx->height-1), avctx->width, avctx->height,
                                    -p->linesize[3], buf, buf_size);
        break;
    default:
        av_assert0(0);
    }

    if (decoded < 0 || decoded > buf_size)
        goto buf_too_small;
    buf_size -= decoded;

    *got_frame      = 1;

    return avpkt->size - buf_size;
buf_too_small:
    av_log(avctx, AV_LOG_ERROR, "Input data too small.\n");
    return AVERROR(EINVAL);
}

static av_cold int decode_init(AVCodecContext *avctx)
{
    LOCOContext * const l = avctx->priv_data;
    int version;

    l->avctx = avctx;
    if (avctx->extradata_size < 12) {
        av_log(avctx, AV_LOG_ERROR, "Extradata size must be >= 12 instead of %i\n",
               avctx->extradata_size);
        return AVERROR_INVALIDDATA;
    }
    version = AV_RL32(avctx->extradata);
    switch (version) {
    case 1:
        l->lossy = 0;
        break;
    case 2:
        l->lossy = AV_RL32(avctx->extradata + 8);
        break;
    default:
        l->lossy = AV_RL32(avctx->extradata + 8);
        avpriv_request_sample(avctx, "LOCO codec version %i", version);
    }

    if (l->lossy > 65536U) {
        av_log(avctx, AV_LOG_ERROR, "lossy %i is too large\n", l->lossy);
        return AVERROR_INVALIDDATA;
    }

    l->mode = AV_RL32(avctx->extradata + 4);
    switch (l->mode) {
    case LOCO_CYUY2:
    case LOCO_YUY2:
    case LOCO_UYVY:
        avctx->pix_fmt = AV_PIX_FMT_YUV422P;
        break;
    case LOCO_CRGB:
    case LOCO_RGB:
        avctx->pix_fmt = AV_PIX_FMT_GBRP;
        break;
    case LOCO_CYV12:
    case LOCO_YV12:
        avctx->pix_fmt = AV_PIX_FMT_YUV420P;
        break;
    case LOCO_CRGBA:
    case LOCO_RGBA:
        avctx->pix_fmt = AV_PIX_FMT_GBRAP;
        break;
    default:
        av_log(avctx, AV_LOG_INFO, "Unknown colorspace, index = %i\n", l->mode);
        return AVERROR_INVALIDDATA;
    }
    if (avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(avctx, AV_LOG_INFO, "lossy:%i, version:%i, mode: %i\n", l->lossy, version, l->mode);

    return 0;
}

const FFCodec ff_loco_decoder = {
    .p.name         = "loco",
    CODEC_LONG_NAME("LOCO"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_LOCO,
    .priv_data_size = sizeof(LOCOContext),
    .init           = decode_init,
    FF_CODEC_DECODE_CB(decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
