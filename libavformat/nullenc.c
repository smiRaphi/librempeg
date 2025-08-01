/*
 * RAW null muxer
 * Copyright (c) 2002 Fabrice Bellard
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

#include "avformat.h"
#include "mux.h"

static int null_write_packet(struct AVFormatContext *s, AVPacket *pkt)
{
    return 0;
}

const FFOutputFormat ff_null_muxer = {
    .p.name            = "null",
    .p.long_name       = NULL_IF_CONFIG_SMALL("raw null video"),
    .p.audio_codec     = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE),
    .p.video_codec     = AV_CODEC_ID_WRAPPED_AVFRAME,
    .write_packet      = null_write_packet,
    .p.flags           = AVFMT_VARIABLE_FPS | AVFMT_NOFILE | AVFMT_NOTIMESTAMPS,
    .interleave_packet = ff_interleave_packet_passthrough,
};
