/*
 * DTPK demuxer
 * Copyright (c) 2025 smiRaphi
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

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "demux.h"
#include "internal.h"

typedef struct DTPKStream {
    int64_t start_offset;
    int64_t end_offset;
} DTPKStream;

static int dtpk_probe(const AVProbeData *p)
{
    if (AV_RL32(p->buf) != MKTAG('D','T','P','K') || AV_RL32(p->buf+12) != 0 || AV_RL32(p->buf+0x1C) != 0)
        return 0;

    if (p->buf_size < 0x40)
        return 0;

    if ((AV_RL32(p->buf+0x3C)+0x14) > AV_RL32(p->buf+8))
        return 0;

    return AVPROBE_SCORE_MAX/3*2;
}

static int sort_streams(const void *a, const void *b)
{
    const AVStream *const *s1p = a;
    const AVStream *const *s2p = b;
    const AVStream *s1 = *s1p;
    const AVStream *s2 = *s2p;
    const DTPKStream *ds1 = s1->priv_data;
    const DTPKStream *ds2 = s2->priv_data;

    return FFDIFFSIGN(ds1->start_offset, ds2->start_offset);
}

static int dtpk_read_header(AVFormatContext *s)
{
    int i;
    int64_t smp_plbk_inf_off, smp_dat_off;
    uint32_t smp_plbk_inf_num;
    AVIOContext *pb = s->pb;

    if (avio_rl32(pb) != MKTAG('D','T','P','K'))
        return AVERROR_INVALIDDATA;
    avio_skip(pb, 0x2C);
    smp_plbk_inf_off = avio_rl32(pb);
    avio_skip(pb, 8);
    smp_dat_off = avio_rl32(pb);

    avio_seek(pb, smp_dat_off, SEEK_SET);
    for (i = 0; i < (avio_rl32(pb)+1); i++) {
        uint32_t flags = avio_rl32(pb);
        uint16_t loop_start = avio_rl16(pb);
        uint16_t loop_end = avio_rl16(pb);
        uint32_t channels = avio_rl32(pb);
        uint32_t size = avio_rl32(pb);

        AVStream *st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);
        DTPKStream *dst = av_mallocz(sizeof(*dst));
        if (!dst)
            return AVERROR(ENOMEM);

        st->start_time = 0;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->priv_data = dst;
        st->codecpar->sample_rate = 0;

        dst->start_offset = flags & 0x7FFFFF;
        if (flags & 0x1000000) {
            if (flags & 0x800000)
                return AVERROR_INVALIDDATA;
            st->codecpar->codec_id = AV_CODEC_ID_ADPCM_AICA;
            st->codecpar->bits_per_coded_sample = 4;
        } else {
            if (flags & 0x800000) {
                st->codecpar->codec_id = AV_CODEC_ID_PCM_U8;
                st->codecpar->bits_per_coded_sample = 8;
            } else {
                st->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
                st->codecpar->bits_per_coded_sample = 16;
            }
        }

        switch (channels) {
        case 0:
            st->codecpar->ch_layout.nb_channels = 1;
            break;
        case 0x80:
            st->codecpar->ch_layout.nb_channels = 2;
            break;
        default:
            avpriv_request_sample(s, "channels %d", channels);
            return AVERROR_PATCHWELCOME;
        }
        dst->end_offset = dst->start_offset + size*st->codecpar->ch_layout.nb_channels;

        st->codecpar->block_align = 0x100 * st->codecpar->ch_layout.nb_channels;
        st->duration = size/st->codecpar->bits_per_coded_sample;
        if (loop_start > 0)
            av_dict_set_int(&st->metadata, "loop_start", loop_start, 0);
        if (loop_end > 0 && loop_end < st->duration)
            av_dict_set_int(&st->metadata, "loop_end", loop_end, 0);
    }

    avio_seek(pb, smp_plbk_inf_off + 0x10, SEEK_SET);
    smp_plbk_inf_num = avio_r8(pb) + 1;
    avio_skip(pb, 0x3F);
    for (i = 0; i < smp_plbk_inf_num; i++) {
        uint8_t smp_id;
        uint16_t rate;
        AVStream *st;

        avio_skip(pb, 2);
        smp_id = avio_r8(pb);
        avio_skip(pb, 7);
        rate = avio_rl32(pb);
        avio_skip(pb, 0x34);

        if (smp_id >= s->nb_streams)
            continue;
        st = s->streams[smp_id];
        if (st->codecpar->sample_rate)
            continue;

        switch (rate) {
        case 0x0000: rate = 44100;break;
        case 0x0100: rate = 45000;break;
        case 0x0200: rate = 46000;break;
        case 0x0300: rate = 47000;break;
        case 0x0400: rate = 48000;break;
        case 0x0500: rate = 49000;break;
        case 0xd61d: rate =  4000;break;
        case 0xdd1e: rate =  6000;break;
        case 0xde36: rate =  6500;break;
        case 0xe008: rate =  7000;break;
        case 0xe21d: rate =  8000;break;
        case 0xe21e: rate =  8012;break;
        case 0xe320: rate =  8500;break;
        case 0xe41f: rate =  9000;break;
        case 0xe51c: rate =  9500;break;
        case 0xe70a: rate = 10500;break;
        case 0xe800: rate = 11025;break;
        case 0xe91e: rate = 12000;break;
        case 0xea0b: rate = 12500;break;
        case 0xea36: rate = 13000;break;
        case 0xec08: rate = 14000;break;
        case 0xed15: rate = 15000;break;
        case 0xee1d: rate = 16000;break;
        case 0xef20: rate = 17000;break;
        case 0xf01f: rate = 18000;break;
        case 0xf11c: rate = 19000;break;
        case 0xf214: rate = 20000;break;
        case 0xf30a: rate = 21000;break;
        case 0xf400: rate = 22050;break;
        case 0xf42f: rate = 23000;break;
        default:
            if (rate > 0x600) {
                avpriv_request_sample(s, "unmapped rate 0x%04X", rate);
                return AVERROR_PATCHWELCOME;
            }

            if      (rate < 0x100) rate = 45000;
            else if (rate < 0x200) rate = 46000;
            else if (rate < 0x300) rate = 47000;
            else if (rate < 0x400) rate = 48000;
            else if (rate < 0x500) rate = 49000;
            else                   rate = 44100;

            break;
        }

        st->codecpar->sample_rate = rate;
    }

    qsort(s->streams, s->nb_streams, sizeof(AVStream *), sort_streams);
    for (int n = 0; n < s->nb_streams; n++) {
        AVStream *st = s->streams[n];

        if (!st->codecpar->sample_rate)
            st->codecpar->sample_rate = 22050; // official default sample rate
        st->index = n;
    }

    {
        DTPKStream *dst = s->streams[0]->priv_data;
        avio_seek(pb, dst->start_offset, SEEK_SET);
    }

    return 0;
}

static int dtpk_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int64_t pos, block_size;
    AVIOContext *pb = s->pb;
    int ret = AVERROR_EOF;

    for (int i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        DTPKStream *dst = st->priv_data;

        if (avio_feof(pb))
            return AVERROR_EOF;

        pos = avio_tell(pb);
        if (pos >= dst->start_offset && pos < dst->end_offset) {
            block_size = FFMIN(dst->end_offset - pos, st->codecpar->block_align);

            ret = av_get_packet(pb, pkt, block_size);
            pkt->pos = pos;
            pkt->stream_index = st->index;

            break;
        } else if (pos >= dst->end_offset && i+1 < s->nb_streams) {
            AVStream *st_next = s->streams[i+1];
            DTPKStream *dst_next = st_next->priv_data;
            if (dst_next->start_offset > pos)
                avio_seek(pb, dst_next->start_offset, SEEK_SET);
        }
    }

    return ret;
}

const FFInputFormat ff_dtpk_demuxer = {
    .p.name         = "dtpk",
    .p.long_name    = NULL_IF_CONFIG_SMALL("AM2 DTPK Bank"),
    .p.extensions   = "snd",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .read_probe     = dtpk_probe,
    .read_header    = dtpk_read_header,
    .read_packet    = dtpk_read_packet,
};
