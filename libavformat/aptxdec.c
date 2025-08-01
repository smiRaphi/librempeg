/*
 * RAW aptX demuxer
 *
 * Copyright (C) 2017  Aurelien Jacobs <aurel@gnuage.org>
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

#include "config_components.h"

#include "libavutil/opt.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define APTX_BLOCK_SIZE   4
#define APTX_PACKET_SIZE  (256*APTX_BLOCK_SIZE)

#define APTX_HD_BLOCK_SIZE   6
#define APTX_HD_PACKET_SIZE  (256*APTX_HD_BLOCK_SIZE)

typedef struct AptXDemuxerContext {
    AVClass *class;
    int sample_rate;
} AptXDemuxerContext;

static AVStream *aptx_read_header_common(AVFormatContext *s)
{
    AptXDemuxerContext *s1 = s->priv_data;
    AVStream *st = avformat_new_stream(s, NULL);
    if (!st)
        return NULL;
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->format = AV_SAMPLE_FMT_S32P;
    st->codecpar->ch_layout.nb_channels = 2;
    st->codecpar->sample_rate = s1->sample_rate;
    st->start_time = 0;
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
    return st;
}

static int aptx_read_header(AVFormatContext *s)
{
    AVStream *st = aptx_read_header_common(s);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_id = AV_CODEC_ID_APTX;
    st->codecpar->bits_per_coded_sample = 4;
    st->codecpar->block_align = APTX_BLOCK_SIZE;
    return 0;
}

static int aptx_hd_read_header(AVFormatContext *s)
{
    AVStream *st = aptx_read_header_common(s);
    if (!st)
        return AVERROR(ENOMEM);
    st->codecpar->codec_id = AV_CODEC_ID_APTX_HD;
    st->codecpar->bits_per_coded_sample = 6;
    st->codecpar->block_align = APTX_HD_BLOCK_SIZE;
    return 0;
}

static int aptx_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret = av_get_packet(s->pb, pkt, APTX_PACKET_SIZE);
    if (ret >= 0 && !(ret % APTX_BLOCK_SIZE))
        pkt->flags &= ~AV_PKT_FLAG_CORRUPT;
    return ret >= 0 ? 0 : ret;
}

static int aptx_hd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret = av_get_packet(s->pb, pkt, APTX_HD_PACKET_SIZE);
    if (ret >= 0 && !(ret % APTX_HD_BLOCK_SIZE))
        pkt->flags &= ~AV_PKT_FLAG_CORRUPT;
    return ret >= 0 ? 0 : ret;
}

static const AVOption aptx_options[] = {
    { "sample_rate", "", offsetof(AptXDemuxerContext, sample_rate), AV_OPT_TYPE_INT, {.i64 = 48000}, 1, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass aptx_demuxer_class = {
    .class_name = "aptx (hd) demuxer",
    .option     = aptx_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

#if CONFIG_APTX_DEMUXER
const FFInputFormat ff_aptx_demuxer = {
    .p.name         = "aptx",
    .p.long_name    = NULL_IF_CONFIG_SMALL("raw aptX"),
    .p.extensions   = "aptx",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .p.priv_class   = &aptx_demuxer_class,
    .priv_data_size = sizeof(AptXDemuxerContext),
    .read_header    = aptx_read_header,
    .read_packet    = aptx_read_packet,
};
#endif

#if CONFIG_APTX_HD_DEMUXER
const FFInputFormat ff_aptx_hd_demuxer = {
    .p.name         = "aptx_hd",
    .p.long_name    = NULL_IF_CONFIG_SMALL("raw aptX HD"),
    .p.extensions   = "aptxhd",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .p.priv_class   = &aptx_demuxer_class,
    .priv_data_size = sizeof(AptXDemuxerContext),
    .read_header    = aptx_hd_read_header,
    .read_packet    = aptx_hd_read_packet,
};
#endif
