/*
 * WavPack muxer
 * Copyright (c) 2013 Konstantin Shishkov <kostya.shishkov@gmail.com>
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

#include "libavutil/attributes.h"

#include "apetag.h"
#include "avformat.h"
#include "mux.h"
#include "wv.h"

typedef struct WvMuxContext {
    int64_t samples;
} WvMuxContext;

static int wv_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    WvMuxContext *s = ctx->priv_data;
    WvHeader header;
    int ret;

    if (pkt->size < WV_HEADER_SIZE ||
        (ret = ff_wv_parse_header(&header, pkt->data)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid WavPack packet.\n");
        return AVERROR(EINVAL);
    }
    s->samples += header.samples;

    avio_write(ctx->pb, pkt->data, pkt->size);

    return 0;
}

static av_cold int wv_write_trailer(AVFormatContext *ctx)
{
    WvMuxContext *s = ctx->priv_data;

    /* update total number of samples in the first block */
    if ((ctx->pb->seekable & AVIO_SEEKABLE_NORMAL) && s->samples &&
        s->samples < UINT32_MAX) {
        int64_t pos = avio_tell(ctx->pb);
        avio_seek(ctx->pb, 12, SEEK_SET);
        avio_wl32(ctx->pb, s->samples);
        avio_seek(ctx->pb, pos, SEEK_SET);
    }

    ff_ape_write_tag(ctx);
    return 0;
}

const FFOutputFormat ff_wv_muxer = {
    .p.name            = "wv",
    .p.long_name       = NULL_IF_CONFIG_SMALL("raw WavPack"),
    .p.mime_type       = "audio/x-wavpack",
    .p.extensions      = "wv",
    .priv_data_size    = sizeof(WvMuxContext),
    .p.audio_codec     = AV_CODEC_ID_WAVPACK,
    .p.video_codec     = AV_CODEC_ID_NONE,
    .p.subtitle_codec  = AV_CODEC_ID_NONE,
    .write_packet      = wv_write_packet,
    .write_trailer     = wv_write_trailer,
    .p.flags           = AVFMT_NOTIMESTAMPS,
    .flags_internal    = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                         FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
};
