/*
 * SMJPEG muxer
 * Copyright (c) 2012 Paul B Mahol
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
 * This is a muxer for Loki SDL Motion JPEG files
 */

#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "smjpeg.h"

typedef struct SMJPEGMuxContext {
    uint32_t duration;
} SMJPEGMuxContext;

static int smjpeg_write_header(AVFormatContext *s)
{
    const AVDictionaryEntry *t = NULL;
    AVIOContext *pb = s->pb;
    int n, tag;

    avio_write(pb, SMJPEG_MAGIC, 8);
    avio_wb32(pb, 0);
    avio_wb32(pb, 0);

    ff_standardize_creation_time(s);
    while ((t = av_dict_iterate(s->metadata, t))) {
        avio_wl32(pb, SMJPEG_TXT);
        avio_wb32(pb, strlen(t->key) + strlen(t->value) + 3);
        avio_write(pb, t->key, strlen(t->key));
        avio_write(pb, " = ", 3);
        avio_write(pb, t->value, strlen(t->value));
    }

    for (n = 0; n < s->nb_streams; n++) {
        AVStream *st = s->streams[n];
        AVCodecParameters *par = st->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            tag = ff_codec_get_tag(ff_codec_smjpeg_audio_tags, par->codec_id);
            if (!tag) {
                av_log(s, AV_LOG_ERROR, "unsupported audio codec\n");
                return AVERROR(EINVAL);
            }
            avio_wl32(pb, SMJPEG_SND);
            avio_wb32(pb, 8);
            avio_wb16(pb, par->sample_rate);
            avio_w8(pb, par->bits_per_coded_sample);
            avio_w8(pb, par->ch_layout.nb_channels);
            avio_wl32(pb, tag);
            avpriv_set_pts_info(st, 32, 1, 1000);
        } else if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            tag = ff_codec_get_tag(ff_codec_smjpeg_video_tags, par->codec_id);
            if (!tag) {
                av_log(s, AV_LOG_ERROR, "unsupported video codec\n");
                return AVERROR(EINVAL);
            }
            avio_wl32(pb, SMJPEG_VID);
            avio_wb32(pb, 12);
            avio_wb32(pb, 0);
            avio_wb16(pb, par->width);
            avio_wb16(pb, par->height);
            avio_wl32(pb, tag);
            avpriv_set_pts_info(st, 32, 1, 1000);
        }
    }

    avio_wl32(pb, SMJPEG_HEND);

    return 0;
}

static int smjpeg_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SMJPEGMuxContext *smc = s->priv_data;
    AVIOContext *pb = s->pb;
    AVStream *st = s->streams[pkt->stream_index];
    AVCodecParameters *par = st->codecpar;

    if (par->codec_type == AVMEDIA_TYPE_AUDIO)
        avio_wl32(pb, SMJPEG_SNDD);
    else if (par->codec_type == AVMEDIA_TYPE_VIDEO)
        avio_wl32(pb, SMJPEG_VIDD);
    else
        return 0;

    avio_wb32(pb, pkt->pts);
    avio_wb32(pb, pkt->size);
    avio_write(pb, pkt->data, pkt->size);

    smc->duration = FFMAX(smc->duration, pkt->pts + pkt->duration);
    return 0;
}

static int smjpeg_write_trailer(AVFormatContext *s)
{
    SMJPEGMuxContext *smc = s->priv_data;
    AVIOContext *pb = s->pb;
    int64_t currentpos;

    if (pb->seekable & AVIO_SEEKABLE_NORMAL) {
        currentpos = avio_tell(pb);
        avio_seek(pb, 12, SEEK_SET);
        avio_wb32(pb, smc->duration);
        avio_seek(pb, currentpos, SEEK_SET);
    }

    avio_wl32(pb, SMJPEG_DONE);

    return 0;
}

const FFOutputFormat ff_smjpeg_muxer = {
    .p.name         = "smjpeg",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Loki SDL MJPEG"),
    .priv_data_size = sizeof(SMJPEGMuxContext),
    .p.audio_codec  = AV_CODEC_ID_PCM_S16LE,
    .p.video_codec  = AV_CODEC_ID_MJPEG,
    .p.subtitle_codec = AV_CODEC_ID_NONE,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH,
    .write_header   = smjpeg_write_header,
    .write_packet   = smjpeg_write_packet,
    .write_trailer  = smjpeg_write_trailer,
    .p.flags        = AVFMT_GLOBALHEADER | AVFMT_TS_NONSTRICT,
    .p.codec_tag    = (const AVCodecTag *const []){ ff_codec_smjpeg_video_tags, ff_codec_smjpeg_audio_tags, 0 },
};
