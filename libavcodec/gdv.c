/*
 * Gremlin Digital Video (GDV) decoder
 * Copyright (c) 2017 Konstantin Shishkov
 * Copyright (c) 2017 Paul B Mahol
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
#include "libavutil/mem.h"
#include "avcodec.h"
#include "bytestream.h"
#include "codec_internal.h"
#include "decode.h"

typedef struct GDVContext {
    AVCodecContext *avctx;

    GetByteContext gb;
    GetByteContext g2;
    PutByteContext pb;

    uint32_t pal[256];
    uint8_t *frame;
    unsigned frame_size;
    unsigned scale_h, scale_v;
} GDVContext;

typedef struct Bits8 {
    uint8_t queue;
    uint8_t fill;
} Bits8;

typedef struct Bits32 {
    uint32_t queue;
    uint8_t  fill;
} Bits32;

#define PREAMBLE_SIZE 4096

static av_cold int gdv_decode_init(AVCodecContext *avctx)
{
    GDVContext *gdv = avctx->priv_data;
    int i, j, k;

    avctx->pix_fmt  = AV_PIX_FMT_PAL8;
    gdv->frame_size = avctx->width * avctx->height + PREAMBLE_SIZE;
    gdv->frame = av_calloc(gdv->frame_size, 1);
    if (!gdv->frame)
        return AVERROR(ENOMEM);

    for (i = 0; i < 2; i++) {
        for (j = 0; j < 256; j++) {
            for (k = 0; k < 8; k++) {
                gdv->frame[i * 2048 + j * 8 + k] = j;
            }
        }
    }

    return 0;
}

static void scaleup(uint8_t *dst, const uint8_t *src, int w)
{
    int x;
    for (x = 0; x < w - 7; x+=8) {
        dst[x + 0] =
        dst[x + 1] = src[(x>>1) + 0];
        dst[x + 2] =
        dst[x + 3] = src[(x>>1) + 1];
        dst[x + 4] =
        dst[x + 5] = src[(x>>1) + 2];
        dst[x + 6] =
        dst[x + 7] = src[(x>>1) + 3];
    }
    for (; x < w; x++) {
        dst[x] = src[(x>>1)];
    }
}

static void scaleup_rev(uint8_t *dst, const uint8_t *src, int w)
{
    int x;

    for (x = w - 1; (x+1) & 7; x--) {
        dst[x] = src[(x>>1)];
    }
    for (x -= 7; x >= 0; x -= 8) {
        dst[x + 6] =
        dst[x + 7] = src[(x>>1) + 3];
        dst[x + 4] =
        dst[x + 5] = src[(x>>1) + 2];
        dst[x + 2] =
        dst[x + 3] = src[(x>>1) + 1];
        dst[x + 0] =
        dst[x + 1] = src[(x>>1) + 0];
    }
}

static void scaledown(uint8_t *dst, const uint8_t *src, int w)
{
    int x;
    for (x = 0; x < w - 7; x+=8) {
        dst[x + 0] = src[2*x + 0];
        dst[x + 1] = src[2*x + 2];
        dst[x + 2] = src[2*x + 4];
        dst[x + 3] = src[2*x + 6];
        dst[x + 4] = src[2*x + 8];
        dst[x + 5] = src[2*x +10];
        dst[x + 6] = src[2*x +12];
        dst[x + 7] = src[2*x +14];
    }
    for (; x < w; x++) {
        dst[x] = src[2*x];
    }
}

static void rescale(GDVContext *gdv, uint8_t *dst, int w, int h, int scale_v, int scale_h)
{
    int j, y;

    if ((gdv->scale_v == scale_v) && (gdv->scale_h == scale_h)) {
        return;
    }

    if (gdv->scale_v) {
        for (j = 0; j < h; j++) {
            int y = h - j - 1;
            uint8_t *dst1 = dst + PREAMBLE_SIZE + y * w;
            uint8_t *src1 = dst + PREAMBLE_SIZE + (y>>!!gdv->scale_h) * (w>>1);

            scaleup_rev(dst1, src1, w);
        }
    } else if (gdv->scale_h) {
        for (j = 0; j < h; j++) {
            int y = h - j - 1;
            uint8_t *dst1 = dst + PREAMBLE_SIZE + y * w;
            uint8_t *src1 = dst + PREAMBLE_SIZE + (y>>1) * w;
            memcpy(dst1, src1, w);
        }
    }

    if (scale_h && scale_v) {
        for (y = 0; y < (h>>1); y++) {
            uint8_t *dst1 = dst + PREAMBLE_SIZE + y * (w>>1);
            uint8_t *src1 = dst + PREAMBLE_SIZE + y*2 * w;
            scaledown(dst1, src1, w>>1);
        }
    } else if (scale_h) {
        for (y = 0; y < (h>>1); y++) {
            uint8_t *dst1 = dst + PREAMBLE_SIZE + y * w;
            uint8_t *src1 = dst + PREAMBLE_SIZE + y*2 * w;
            memcpy(dst1, src1, w);
        }
    } else if (scale_v) {
        for (y = 0; y < h; y++) {
            uint8_t *dst1 = dst + PREAMBLE_SIZE + y * w;
            scaledown(dst1, dst1, w>>1);
        }
    }

    gdv->scale_v = scale_v;
    gdv->scale_h = scale_h;
}

static int read_bits2(Bits8 *bits, GetByteContext *gb)
{
    int res;

    if (bits->fill == 0) {
        bits->queue |= bytestream2_get_byte(gb);
        bits->fill   = 8;
    }
    res = bits->queue >> 6;
    bits->queue <<= 2;
    bits->fill   -= 2;

    return res;
}

static void fill_bits32(Bits32 *bits, GetByteContext *gb)
{
    bits->queue = bytestream2_get_le32(gb);
    bits->fill  = 32;
}

static int read_bits32(Bits32 *bits, GetByteContext *gb, int nbits)
{
    int res = bits->queue & ((1 << nbits) - 1);

    bits->queue >>= nbits;
    bits->fill   -= nbits;
    if (bits->fill <= 16) {
        bits->queue |= bytestream2_get_le16(gb) << bits->fill;
        bits->fill  += 16;
    }

    return res;
}

static void lz_copy(PutByteContext *pb, GetByteContext *g2, int offset, unsigned len)
{
    int i;

    if (offset == -1) {
        int c;

        bytestream2_seek(g2, bytestream2_tell_p(pb) - 1, SEEK_SET);
        c = bytestream2_get_byte(g2);
        for (i = 0; i < len; i++) {
            bytestream2_put_byte(pb, c);
        }
    } else if (offset < 0) {
        int start = bytestream2_tell_p(pb) - (-offset);

        bytestream2_seek(g2, start, SEEK_SET);
        for (i = 0; i < len; i++) {
            bytestream2_put_byte(pb, bytestream2_get_byte(g2));
        }
    } else {
        int start = bytestream2_tell_p(pb) + offset;

        bytestream2_seek(g2, start, SEEK_SET);
        for (i = 0; i < len; i++) {
            bytestream2_put_byte(pb, bytestream2_get_byte(g2));
        }
    }
}

static int decompress_2(AVCodecContext *avctx)
{
    GDVContext *gdv = avctx->priv_data;
    GetByteContext *gb = &gdv->gb;
    GetByteContext *g2 = &gdv->g2;
    PutByteContext *pb = &gdv->pb;
    Bits8 bits = { 0 };
    int c, i;

    bytestream2_init(g2, gdv->frame, gdv->frame_size);
    bytestream2_skip_p(pb, PREAMBLE_SIZE);

    for (c = 0; c < 256; c++) {
        for (i = 0; i < 16; i++) {
            gdv->frame[c * 16 + i] = c;
        }
    }

    while (bytestream2_get_bytes_left_p(pb) > 0 && bytestream2_get_bytes_left(gb) > 0) {
        int tag = read_bits2(&bits, gb);
        if (tag == 0) {
            bytestream2_put_byte(pb, bytestream2_get_byte(gb));
        } else if (tag == 1) {
            int b = bytestream2_get_byte(gb);
            int len = (b & 0xF) + 3;
            int top = (b >> 4) & 0xF;
            int off = (bytestream2_get_byte(gb) << 4) + top - 4096;
            lz_copy(pb, g2, off, len);
        } else if (tag == 2) {
            int len = (bytestream2_get_byte(gb)) + 2;
            bytestream2_skip_p(pb, len);
        } else {
            break;
        }
    }

    if (bytestream2_get_bytes_left_p(pb) > 0)
        return AVERROR_INVALIDDATA;

    return 0;
}

static int decompress_5(AVCodecContext *avctx, unsigned skip)
{
    GDVContext *gdv = avctx->priv_data;
    GetByteContext *gb = &gdv->gb;
    GetByteContext *g2 = &gdv->g2;
    PutByteContext *pb = &gdv->pb;
    Bits8 bits = { 0 };

    bytestream2_init(g2, gdv->frame, gdv->frame_size);
    bytestream2_skip_p(pb, skip + PREAMBLE_SIZE);

    while (bytestream2_get_bytes_left_p(pb) > 0 && bytestream2_get_bytes_left(gb) > 0) {
        int tag = read_bits2(&bits, gb);
        if (bytestream2_get_bytes_left(gb) < 1)
            return AVERROR_INVALIDDATA;
        if (tag == 0) {
            bytestream2_put_byte(pb, bytestream2_get_byte(gb));
        } else if (tag == 1) {
            int b = bytestream2_get_byte(gb);
            int len = (b & 0xF) + 3;
            int top = b >> 4;
            int off = (bytestream2_get_byte(gb) << 4) + top - 4096;
            lz_copy(pb, g2, off, len);
        } else if (tag == 2) {
            int len;
            int b = bytestream2_get_byte(gb);
            if (b == 0) {
                return 0;
            }
            if (b != 0xFF) {
                len = b;
            } else {
                len = bytestream2_get_le16(gb);
            }
            bytestream2_skip_p(pb, len + 1);
        } else {
            int b = bytestream2_get_byte(gb);
            int len = (b & 0x3) + 2;
            int off = -(b >> 2) - 1;
            lz_copy(pb, g2, off, len);
        }
    }
    if (bytestream2_get_bytes_left_p(pb) > 0)
        return AVERROR_INVALIDDATA;
    return 0;
}

static int decompress_68(AVCodecContext *avctx, unsigned skip, unsigned use8)
{
    GDVContext *gdv = avctx->priv_data;
    GetByteContext *gb = &gdv->gb;
    GetByteContext *g2 = &gdv->g2;
    PutByteContext *pb = &gdv->pb;
    Bits32 bits;

    bytestream2_init(g2, gdv->frame, gdv->frame_size);
    bytestream2_skip_p(pb, skip + PREAMBLE_SIZE);
    fill_bits32(&bits, gb);

    while (bytestream2_get_bytes_left_p(pb) > 0 && bytestream2_get_bytes_left(gb) > 0) {
        int tag = read_bits32(&bits, gb, 2);
        if (tag == 0) {
            int b = read_bits32(&bits, gb, 1);
            if (b == 0) {
                bytestream2_put_byte(pb, bytestream2_get_byte(gb));
            } else {
                int i, len = 2;
                int lbits = 0;
                while (1) {
                    int val;

                    lbits += 1;
                    val = read_bits32(&bits, gb, lbits);
                    len += val;
                    if (val != ((1 << lbits) - 1)) {
                        break;
                    }
                    if (lbits >= 16)
                        return AVERROR_INVALIDDATA;
                }
                for (i = 0; i < len; i++) {
                    bytestream2_put_byte(pb, bytestream2_get_byte(gb));
                }
            }
        } else if (tag == 1) {
            int b = read_bits32(&bits, gb, 1);
            int len;

            if (b == 0) {
                len = (read_bits32(&bits, gb, 4)) + 2;
            } else {
                int bb = bytestream2_get_byte(gb);
                if ((bb & 0x80) == 0) {
                    len = bb + 18;
                } else {
                    int top = (bb & 0x7F) << 8;
                    len = top + bytestream2_get_byte(gb) + 146;
                }
            }
            bytestream2_skip_p(pb, len);
        } else if (tag == 2) {
            int i, subtag = read_bits32(&bits, gb, 2);

            if (subtag != 3) {
                int top = (read_bits32(&bits, gb, 4)) << 8;
                int offs = top + bytestream2_get_byte(gb);
                if ((subtag != 0) || (offs <= 0xF80)) {
                    int len = (subtag) + 3;
                    lz_copy(pb, g2, (offs) - 4096, len);
                } else {
                    int real_off, len, c1, c2;

                    if (offs == 0xFFF) {
                        return 0;
                    }

                    real_off = ((offs >> 4) & 0x7) + 1;
                    len = ((offs & 0xF) + 2) * 2;
                    c1 = gdv->frame[bytestream2_tell_p(pb) - real_off];
                    c2 = gdv->frame[bytestream2_tell_p(pb) - real_off + 1];
                    for (i = 0; i < len/2; i++) {
                        bytestream2_put_byte(pb, c1);
                        bytestream2_put_byte(pb, c2);
                    }
                }
            } else {
                int b = bytestream2_get_byte(gb);
                int off = ((b & 0x7F)) + 1;
                int len = ((b & 0x80) == 0) ? 2 : 3;

                lz_copy(pb, g2, -off, len);
            }
        } else {
            int len;
            int off;
            if (use8) {
                int q, b = bytestream2_get_byte(gb);
                if ((b & 0xC0) == 0xC0) {
                    len = ((b & 0x3F)) + 8;
                    q = read_bits32(&bits, gb, 4);
                    off = (q << 8) + (bytestream2_get_byte(gb)) + 1;
                } else {
                    int ofs1;
                    if ((b & 0x80) == 0) {
                        len = ((b >> 4)) + 6;
                        ofs1 = (b & 0xF);
                    } else {
                        len = ((b & 0x3F)) + 14;
                        ofs1 = read_bits32(&bits, gb, 4);
                    }
                    off = (ofs1 << 8) + (bytestream2_get_byte(gb)) - 4096;
                }
            } else {
                int ofs1, b = bytestream2_get_byte(gb);

                if ((b >> 4) == 0xF) {
                    len = bytestream2_get_byte(gb) + 21;
                } else {
                    len = (b >> 4) + 6;
                }
                ofs1 = (b & 0xF);
                off = (ofs1 << 8) + bytestream2_get_byte(gb) - 4096;
            }
            lz_copy(pb, g2, off, len);
        }
    }

    if (bytestream2_get_bytes_left_p(pb) > 0)
        return AVERROR_INVALIDDATA;

    return 0;
}

static int gdv_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame, AVPacket *avpkt)
{
    GDVContext *gdv = avctx->priv_data;
    GetByteContext *gb = &gdv->gb;
    PutByteContext *pb = &gdv->pb;
    int ret, i;
    int compression;
    unsigned flags;
    uint8_t *dst;

    bytestream2_init(gb, avpkt->data, avpkt->size);
    bytestream2_init_writer(pb, gdv->frame, gdv->frame_size);

    flags = bytestream2_get_le32(gb);
    compression = flags & 0xF;

    if (compression == 4 || compression == 7 || compression > 8)
        return AVERROR_INVALIDDATA;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    ff_copy_palette(gdv->pal, avpkt, avctx);

    if (compression < 2 && bytestream2_get_bytes_left(gb) < 256*3)
        return AVERROR_INVALIDDATA;
    rescale(gdv, gdv->frame, avctx->width, avctx->height,
            !!(flags & 0x10), !!(flags & 0x20));

    switch (compression) {
    case 1:
        memset(gdv->frame + PREAMBLE_SIZE, 0, gdv->frame_size - PREAMBLE_SIZE);
    case 0:
        for (i = 0; i < 256; i++) {
            unsigned r = bytestream2_get_byte(gb);
            unsigned g = bytestream2_get_byte(gb);
            unsigned b = bytestream2_get_byte(gb);
            gdv->pal[i] = 0xFFU << 24 | r << 18 | g << 10 | b << 2;
        }
        break;
    case 2:
        ret = decompress_2(avctx);
        break;
    case 3:
        break;
    case 5:
        ret = decompress_5(avctx, flags >> 8);
        break;
    case 6:
        ret = decompress_68(avctx, flags >> 8, 0);
        break;
    case 8:
        ret = decompress_68(avctx, flags >> 8, 1);
        break;
    default:
        av_assert0(0);
    }
    if (ret < 0)
        return ret;

    memcpy(frame->data[1], gdv->pal, AVPALETTE_SIZE);
    dst = frame->data[0];

    if (!gdv->scale_v && !gdv->scale_h) {
        int sidx = PREAMBLE_SIZE, didx = 0;
        int y;

        for (y = 0; y < avctx->height; y++) {
            memcpy(dst + didx, gdv->frame + sidx, avctx->width);
            sidx += avctx->width;
            didx += frame->linesize[0];
        }
    } else {
        int sidx = PREAMBLE_SIZE, didx = 0;
        int y;

        for (y = 0; y < avctx->height; y++) {
            if (!gdv->scale_v) {
                memcpy(dst + didx, gdv->frame + sidx, avctx->width);
            } else {
                uint8_t *dst2 = dst + didx;
                uint8_t *src2 = gdv->frame + sidx;

                scaleup(dst2, src2, avctx->width);
            }
            if (!gdv->scale_h || ((y & 1) == 1)) {
                sidx += !gdv->scale_v ? avctx->width : avctx->width/2;
            }
            didx += frame->linesize[0];
        }
    }

    *got_frame = 1;

    return avpkt->size;
}

static av_cold int gdv_decode_close(AVCodecContext *avctx)
{
    GDVContext *gdv = avctx->priv_data;
    av_freep(&gdv->frame);
    return 0;
}

const FFCodec ff_gdv_decoder = {
    .p.name         = "gdv",
    CODEC_LONG_NAME("Gremlin Digital Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_GDV,
    .priv_data_size = sizeof(GDVContext),
    .init           = gdv_decode_init,
    .close          = gdv_decode_close,
    FF_CODEC_DECODE_CB(gdv_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
