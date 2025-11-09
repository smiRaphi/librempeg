/*
 * 3DO Bank demuxer
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

typedef struct ThreeDOBankStream {
    int64_t start_offset;
    int64_t stop_offset;
} ThreeDOBankStream;

static int probe(const AVProbeData *p)
{
    int i;
    int64_t off = 4;
    int64_t tsize = 0;
    uint32_t size = AV_RB32(p->buf);
    if (size < 0x12)
        return 0;

    for (i = 1; i < (INT32_MAX/0x40); i++) {
        if (AV_RB32(p->buf + off) == -1)
            break;
        if (AV_RB32(p->buf + off) <= 0)
            return 0;
        off += 4;
        tsize += 0x40;

        tsize += AV_RB32(p->buf + off);
        off += 0x20;

        off += 8;
        if (memcmp(p->buf + off,"CBD2",4) && memcmp(p->buf + off,"ADP4",4))
            return 0;
        off += 4;

        if (AV_RB32(p->buf + off) == 0x1F) {
            off += 0x10;
            tsize += 0x10;
        }
        off += 0x10;
    }
    if (i == 1)
        return 0;

    tsize += 4;

    if (tsize != size)
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int read_header(AVFormatContext *s)
{
    int64_t data_end, offset;
    AVIOContext *pb = s->pb;
    ThreeDOBankStream *tst;
    AVStream *st;

    avio_skip(pb, 4);

    for (int i = 1; i < INT32_MAX; i++) {
        uint32_t codec;

        if (avio_rb32(pb) == -1) {
            if (i == 1)
                return AVERROR_EOF;
            break;
        }

        st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        tst = av_mallocz(sizeof(*tst));
        if (!tst)
            return AVERROR(ENOMEM);

        st->start_time = 0;
        st->priv_data = tst;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;

        tst->stop_offset = avio_rb32(pb);
        avio_skip(pb, 12);

        st->duration = avio_rb32(pb);
        avio_skip(pb, 12);

        st->codecpar->block_align = avio_rb32(pb) * 0x10;
        avio_skip(pb, 4);

        codec = avio_rb32(pb);
        switch (codec)
        {
        case MKBETAG('C','B','D','2'):
            st->codecpar->codec_id = AV_CODEC_ID_CBD2_DPCM;
            break;
        case MKBETAG('A','D','P','4'):
            st->codecpar->codec_id = AV_CODEC_ID_ADPCM_IMA_WS;
            break;
        default:
            avpriv_request_sample(s, "codec %x", codec);
            return AVERROR_PATCHWELCOME;
        }

        if (avio_rb32(pb) == 0x1F) {
            st->codecpar->ch_layout.nb_channels = 2;
            st->codecpar->block_align *= 2;
            avio_skip(pb, 0x10);
        } else {
            st->codecpar->ch_layout.nb_channels = 1;
        }

        avio_skip(pb, 1);
        st->codecpar->sample_rate = avio_rb16(pb);
        avio_skip(pb, 1 + 8);
    }

    offset = avio_tell(pb);
    for (int n = 0; n < s->nb_streams; n++) {
        st = s->streams[n];
        tst = st->priv_data;

        tst->start_offset = offset;
        offset += tst->stop_offset;
        tst->stop_offset = tst->start_offset;

        switch (st->codecpar->codec_id)
        {
        case AV_CODEC_ID_CBD2_DPCM:
            tst->stop_offset += st->duration;
            break;
        case AV_CODEC_ID_ADPCM_IMA_WS:
            tst->stop_offset += st->duration/2 + st->duration % 2;
            break;
        }

        avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);
    }

    {
        AVStream *st = s->streams[0];
        ThreeDOBankStream *tst = st->priv_data;

        avio_seek(pb, tst->start_offset, SEEK_SET);
    }

    return 0;
}

static int read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int64_t pos, block_size;
    AVIOContext *pb = s->pb;
    int ret = AVERROR_EOF;

    for (int i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        ThreeDOBankStream *tst = st->priv_data;

        if (avio_feof(pb))
            return AVERROR_EOF;

        pos = avio_tell(pb);
        if (pos >= tst->start_offset && pos < tst->stop_offset) {
            block_size = FFMIN(tst->stop_offset - pos, st->codecpar->block_align);

            ret = av_get_packet(pb, pkt, block_size);
            pkt->pos = pos;
            pkt->stream_index = st->index;
            break;
        } else if (pos >= tst->stop_offset && i+1 < s->nb_streams) {
            AVStream *st_next = s->streams[i+1];
            ThreeDOBankStream *tst_next = st_next->priv_data;
            if (tst_next->start_offset > pos)
                avio_seek(pb, tst_next->start_offset, SEEK_SET);
        }
    }

    return ret;
}

const FFInputFormat ff_threedobank_demuxer = {
    .p.name         = "3dobank",
    .p.long_name    = NULL_IF_CONFIG_SMALL("3DO Bank"),
    .p.extensions   = "bin",
    .read_probe     = probe,
    .read_header    = read_header,
    .read_packet    = read_packet,
};
