/*
 * Bethesda VID video decoder
 * Copyright (C) 2007 Nicholas Tung
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
 * @brief Bethesda Softworks VID Video Decoder
 * @author Nicholas Tung [ntung (at. ntung com] (2007-03)
 * @see http://wiki.multimedia.cx/index.php?title=Bethsoft_VID
 * @see http://www.svatopluk.com/andux/docs/dfvid.html
 */

#include "libavutil/common.h"
#include "avcodec.h"
#include "bethsoftvideo.h"
#include "bytestream.h"
#include "codec_internal.h"
#include "decode.h"

typedef struct BethsoftvidContext {
    AVFrame *frame;
    GetByteContext g;
} BethsoftvidContext;

static av_cold int bethsoftvid_decode_init(AVCodecContext *avctx)
{
    BethsoftvidContext *vid = avctx->priv_data;
    avctx->pix_fmt = AV_PIX_FMT_PAL8;

    vid->frame = av_frame_alloc();
    if (!vid->frame)
        return AVERROR(ENOMEM);

    return 0;
}

static int set_palette(BethsoftvidContext *ctx, GetByteContext *g)
{
    uint32_t *palette = (uint32_t *)ctx->frame->data[1];
    int a;

    if (bytestream2_get_bytes_left(g) < 256*3)
        return AVERROR_INVALIDDATA;

    for(a = 0; a < 256; a++){
        palette[a] = 0xFFU << 24 | bytestream2_get_be24u(g) * 4;
        palette[a] |= palette[a] >> 6 & 0x30303;
    }
    return 0;
}

static int bethsoftvid_decode_frame(AVCodecContext *avctx, AVFrame *rframe,
                                    int *got_frame, AVPacket *avpkt)
{
    BethsoftvidContext * vid = avctx->priv_data;
    char block_type;
    uint8_t * dst;
    uint8_t * frame_end;
    int remaining = avctx->width;          // number of bytes remaining on a line
    int wrap_to_next_line;
    int code, ret;
    int yoffset;

    bytestream2_init(&vid->g, avpkt->data, avpkt->size);
    block_type = bytestream2_get_byte(&vid->g);
    if (block_type < 1 || block_type > 4)
        return AVERROR_INVALIDDATA;

    if ((ret = ff_reget_buffer(avctx, vid->frame, 0)) < 0)
        return ret;
    wrap_to_next_line = vid->frame->linesize[0] - avctx->width;

    if (avpkt->side_data_elems > 0 &&
        avpkt->side_data[0].type == AV_PKT_DATA_PALETTE) {
        GetByteContext g;
        bytestream2_init(&g, avpkt->side_data[0].data,
                         avpkt->side_data[0].size);
        if ((ret = set_palette(vid, &g)) < 0)
            return ret;
    }

    dst = vid->frame->data[0];
    frame_end = vid->frame->data[0] + vid->frame->linesize[0] * avctx->height;

    switch(block_type){
        case PALETTE_BLOCK: {
            *got_frame = 0;
            if ((ret = set_palette(vid, &vid->g)) < 0) {
                av_log(avctx, AV_LOG_ERROR, "error reading palette\n");
                return ret;
            }
            return bytestream2_tell(&vid->g);
        }
        case VIDEO_YOFF_P_FRAME:
            yoffset = bytestream2_get_le16(&vid->g);
            if(yoffset >= avctx->height)
                return AVERROR_INVALIDDATA;
            dst += vid->frame->linesize[0] * yoffset;
        case VIDEO_P_FRAME:
        case VIDEO_I_FRAME:
            break;
        default:
            return AVERROR_INVALIDDATA;
    }

    // main code
    while((code = bytestream2_get_byte(&vid->g))){
        int length = code & 0x7f;

        // copy any bytes starting at the current position, and ending at the frame width
        while(length > remaining){
            if(code < 0x80)
                bytestream2_get_buffer(&vid->g, dst, remaining);
            else if(block_type == VIDEO_I_FRAME)
                memset(dst, bytestream2_peek_byte(&vid->g), remaining);
            length -= remaining;      // decrement the number of bytes to be copied
            dst += remaining + wrap_to_next_line;    // skip over extra bytes at end of frame
            remaining = avctx->width;
            if(dst == frame_end)
                goto end;
        }

        // copy any remaining bytes after / if line overflows
        if(code < 0x80)
            bytestream2_get_buffer(&vid->g, dst, length);
        else if(block_type == VIDEO_I_FRAME)
            memset(dst, bytestream2_get_byte(&vid->g), length);
        remaining -= length;
        dst += length;
    }
    end:

    if ((ret = av_frame_ref(rframe, vid->frame)) < 0)
        return ret;

    *got_frame = 1;

    return avpkt->size;
}

static av_cold int bethsoftvid_decode_end(AVCodecContext *avctx)
{
    BethsoftvidContext * vid = avctx->priv_data;
    av_frame_free(&vid->frame);
    return 0;
}

const FFCodec ff_bethsoftvid_decoder = {
    .p.name         = "bethsoftvid",
    CODEC_LONG_NAME("Bethesda VID video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_BETHSOFTVID,
    .priv_data_size = sizeof(BethsoftvidContext),
    .init           = bethsoftvid_decode_init,
    .close          = bethsoftvid_decode_end,
    FF_CODEC_DECODE_CB(bethsoftvid_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
