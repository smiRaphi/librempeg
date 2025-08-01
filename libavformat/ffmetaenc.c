/*
 * Metadata muxer
 * Copyright (c) 2010 Anton Khirnov
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

#include <inttypes.h>

#include "avformat.h"
#include "ffmeta.h"
#include "mux.h"
#include "libavutil/dict.h"


static void write_escape_str(AVIOContext *s, const uint8_t *str)
{
    const uint8_t *p = str;

    while (*p) {
        if (*p == '#' || *p == ';' || *p == '=' || *p == '\\' || *p == '\n')
            avio_w8(s, '\\');
        avio_w8(s, *p);
        p++;
    }
}

static void write_tags(AVIOContext *s, AVDictionary *m)
{
    const AVDictionaryEntry *t = NULL;
    while ((t = av_dict_iterate(m, t))) {
        write_escape_str(s, t->key);
        avio_w8(s, '=');
        write_escape_str(s, t->value);
        avio_w8(s, '\n');
    }
}

static int write_header(AVFormatContext *s)
{
    avio_write(s->pb, ID_STRING, sizeof(ID_STRING) - 1);
    avio_w8(s->pb, '1');          // version
    avio_w8(s->pb, '\n');
    return 0;
}

static int write_trailer(AVFormatContext *s)
{
    int i;

    write_tags(s->pb, s->metadata);

    for (i = 0; i < s->nb_streams; i++) {
        avio_write(s->pb, ID_STREAM, sizeof(ID_STREAM) - 1);
        avio_w8(s->pb, '\n');
        write_tags(s->pb, s->streams[i]->metadata);
    }

    for (i = 0; i < s->nb_chapters; i++) {
        AVChapter *ch = s->chapters[i];
        avio_write(s->pb, ID_CHAPTER, sizeof(ID_CHAPTER) - 1);
        avio_w8(s->pb, '\n');
        avio_printf(s->pb, "TIMEBASE=%d/%d\n", ch->time_base.num, ch->time_base.den);
        avio_printf(s->pb, "START=%"PRId64"\n", ch->start);
        avio_printf(s->pb, "END=%"PRId64"\n",   ch->end);
        write_tags(s->pb, ch->metadata);
    }

    return 0;
}

static int write_packet(AVFormatContext *s, AVPacket *pkt)
{
    return 0;
}

const FFOutputFormat ff_ffmetadata_muxer = {
    .p.name        = "ffmetadata",
    .p.long_name   = NULL_IF_CONFIG_SMALL("FFmpeg metadata in text"),
    .p.extensions  = "ffmeta",
    .write_header  = write_header,
    .write_packet  = write_packet,
    .write_trailer = write_trailer,
    .p.flags       = AVFMT_NOTIMESTAMPS | AVFMT_NOSTREAMS,
};
