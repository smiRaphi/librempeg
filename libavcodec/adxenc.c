/*
 * ADX ADPCM codecs
 * Copyright (c) 2001,2003 BERO
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
#include "adx.h"
#include "bytestream.h"
#include "codec_internal.h"
#include "encode.h"
#include "put_bits.h"

/**
 * @file
 * SEGA CRI adx codecs.
 *
 * Reference documents:
 * http://ku-www.ss.titech.ac.jp/~yatsushi/adx.html
 * adx2wav & wav2adx http://www.geocities.co.jp/Playtown/2004/
 */

static void adx_encode(ADXContext *c, uint8_t *adx, const int16_t *wav,
                       ADXChannelState *prev)
{
    const int c0 = -c->coeff[0];
    const int c1 = -c->coeff[1];
    PutBitContext pb;
    int scale;
    int s0, s1, s2, d;
    int max = 0;
    int min = 0;

    s1 = prev->s1;
    s2 = prev->s2;
    for (int i = 0, j = 0; j < 32; i++, j++) {
        s0 = wav[i];
        d = s0 + ((c0 * s1 + c1 * s2) >> COEFF_BITS);
        if (max < d)
            max = d;
        if (min > d)
            min = d;
        s2 = s1;
        s1 = s0;
    }

    if (max == 0 && min == 0) {
        prev->s1 = s1;
        prev->s2 = s2;
        memset(adx, 0, BLOCK_SIZE);
        return;
    }

    if (max / 7 > -min / 8)
        scale = max / 7;
    else
        scale = -min / 8;

    if (scale == 0)
        scale = 1;

    AV_WB16(adx, scale);

    init_put_bits(&pb, adx + 2, 16);

    s1 = prev->s1;
    s2 = prev->s2;
    for (int i = 0, j = 0; j < 32; i++, j++) {
        d = wav[i] + ((c0 * s1 + c1 * s2) >> COEFF_BITS);

        d = av_clip_intp2(ROUNDED_DIV(d, scale), 3);

        put_sbits(&pb, 4, d);

        s0 = d * scale + ((-c0 * s1 + -c1 * s2) >> COEFF_BITS);
        s2 = s1;
        s1 = s0;
    }
    prev->s1 = s1;
    prev->s2 = s2;

    flush_put_bits(&pb);
}

#define HEADER_SIZE 36

static int adx_encode_header(AVCodecContext *avctx, uint8_t *buf, int bufsize)
{
    ADXContext *c = avctx->priv_data;

    bytestream_put_be16(&buf, 0x8000);              /* header signature */
    bytestream_put_be16(&buf, HEADER_SIZE - 4);     /* copyright offset */
    bytestream_put_byte(&buf, 3);                   /* encoding */
    bytestream_put_byte(&buf, BLOCK_SIZE);          /* block size */
    bytestream_put_byte(&buf, 4);                   /* sample size */
    bytestream_put_byte(&buf, avctx->ch_layout.nb_channels); /* channels */
    bytestream_put_be32(&buf, avctx->sample_rate);  /* sample rate */
    bytestream_put_be32(&buf, 0);                   /* total sample count */
    bytestream_put_be16(&buf, c->cutoff);           /* cutoff frequency */
    bytestream_put_byte(&buf, 3);                   /* version */
    bytestream_put_byte(&buf, 0);                   /* flags */
    bytestream_put_be32(&buf, 0);                   /* unknown */
    bytestream_put_be32(&buf, 0);                   /* loop enabled */
    bytestream_put_be16(&buf, 0);                   /* padding */
    bytestream_put_buffer(&buf, "(c)CRI", 6);       /* copyright signature */

    return HEADER_SIZE;
}

static av_cold int adx_encode_init(AVCodecContext *avctx)
{
    ADXContext *c = avctx->priv_data;

    if (avctx->ch_layout.nb_channels > 2) {
        av_log(avctx, AV_LOG_ERROR, "Invalid number of channels\n");
        return AVERROR(EINVAL);
    }
    avctx->frame_size = BLOCK_SAMPLES;

    /* the cutoff can be adjusted, but this seems to work pretty well */
    c->cutoff = 500;
    ff_adx_calculate_coeffs(c->cutoff, avctx->sample_rate, COEFF_BITS, c->coeff);

    return 0;
}

static int adx_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                            const AVFrame *frame, int *got_packet_ptr)
{
    ADXContext *c          = avctx->priv_data;
    uint8_t *dst;
    int channels = avctx->ch_layout.nb_channels;
    int ch, out_size, ret;

    if (!frame) {
        if (c->eof)
            return 0;
        if ((ret = ff_get_encode_buffer(avctx, avpkt, 18, 0)) < 0)
            return ret;
        c->eof = 1;
        dst = avpkt->data;
        bytestream_put_be16(&dst, 0x8001);
        bytestream_put_be16(&dst, 0x000E);
        bytestream_put_be64(&dst, 0x0);
        bytestream_put_be32(&dst, 0x0);
        bytestream_put_be16(&dst, 0x0);
        *got_packet_ptr = 1;
        return 0;
    }

    out_size = BLOCK_SIZE * channels + !c->header_parsed * HEADER_SIZE;
    if ((ret = ff_get_encode_buffer(avctx, avpkt, out_size, 0)) < 0)
        return ret;
    dst = avpkt->data;

    if (!c->header_parsed) {
        int hdrsize;
        if ((hdrsize = adx_encode_header(avctx, dst, avpkt->size)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "output buffer is too small\n");
            return AVERROR(EINVAL);
        }
        dst      += hdrsize;
        c->header_parsed = 1;
    }

    for (ch = 0; ch < channels; ch++) {
        const int16_t *samples = (const int16_t *)frame->extended_data[ch];

        adx_encode(c, dst, samples, &c->prev[ch]);
        dst += BLOCK_SIZE;
    }

    *got_packet_ptr = 1;
    return 0;
}

const FFCodec ff_adpcm_adx_encoder = {
    .p.name         = "adpcm_adx",
    CODEC_LONG_NAME("SEGA CRI ADX ADPCM"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ADPCM_ADX,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY |
                      AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(ADXContext),
    .init           = adx_encode_init,
    FF_CODEC_ENCODE_CB(adx_encode_frame),
    CODEC_SAMPLEFMTS(AV_SAMPLE_FMT_S16P),
    .caps_internal  = FF_CODEC_CAP_EOF_FLUSH,
};
