/*
 * MPEG Audio header decoder
 * Copyright (c) 2001, 2002 Fabrice Bellard
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
 * MPEG Audio header decoder.
 */

#include "libavutil/macros.h"

#include "mpegaudio.h"
#include "mpegaudiodata.h"
#include "mpegaudiodecheader.h"


int avpriv_mpegaudio_decode_header(MPADecodeHeader *s, uint32_t header)
{
    int sample_rate, frame_size, mpeg25, padding;
    int sample_rate_index, bitrate_index;
    int ret;

    ret = ff_mpa_check_header(header);
    if (ret < 0)
        return ret;

    if (header & (1<<20)) {
        s->lsf = (header & (1<<19)) ? 0 : 1;
        mpeg25 = 0;
    } else {
        s->lsf = 1;
        mpeg25 = 1;
    }

    s->layer = 4 - ((header >> 17) & 3);
    /* extract frequency */
    sample_rate_index = (header >> 10) & 3;
    if (sample_rate_index >= FF_ARRAY_ELEMS(ff_mpa_freq_tab))
        sample_rate_index = 0;
    sample_rate = ff_mpa_freq_tab[sample_rate_index] >> (s->lsf + mpeg25);
    sample_rate_index += 3 * (s->lsf + mpeg25);
    s->sample_rate_index = sample_rate_index;
    s->error_protection = ((header >> 16) & 1) ^ 1;
    s->sample_rate = sample_rate;

    bitrate_index = (header >> 12) & 0xf;
    padding = (header >> 9) & 1;
    //extension = (header >> 8) & 1;
    s->mode = (header >> 6) & 3;
    s->mode_ext = (header >> 4) & 3;
    //copyright = (header >> 3) & 1;
    //original = (header >> 2) & 1;
    //emphasis = header & 3;

    if (s->mode == MPA_MONO)
        s->nb_channels = 1;
    else
        s->nb_channels = 2;

    if (bitrate_index != 0) {
        frame_size = ff_mpa_bitrate_tab[s->lsf][s->layer - 1][bitrate_index];
        s->bit_rate = frame_size * 1000;
        switch(s->layer) {
        case 1:
            frame_size = (frame_size * 12000) / sample_rate;
            frame_size = (frame_size + padding) * 4;
            break;
        case 2:
            frame_size = (frame_size * 144000) / sample_rate;
            frame_size += padding;
            break;
        default:
        case 3:
            frame_size = (frame_size * 144000) / (sample_rate << s->lsf);
            frame_size += padding;
            break;
        }
        s->frame_size = frame_size;
    } else {
        /* if no frame size computed, signal it */
        return 1;
    }

#if defined(DEBUG)
    ff_dlog(NULL, "layer%d, %d Hz, %d kbits/s, ",
           s->layer, s->sample_rate, s->bit_rate);
    if (s->nb_channels == 2) {
        if (s->layer == 3) {
            if (s->mode_ext & MODE_EXT_MS_STEREO)
                ff_dlog(NULL, "ms-");
            if (s->mode_ext & MODE_EXT_I_STEREO)
                ff_dlog(NULL, "i-");
        }
        ff_dlog(NULL, "stereo");
    } else {
        ff_dlog(NULL, "mono");
    }
    ff_dlog(NULL, "\n");
#endif
    return 0;
}

int ff_mpa_decode_header(uint32_t head, int *sample_rate, int *channels, int *frame_size, int *bit_rate, enum AVCodecID *codec_id)
{
    MPADecodeHeader s1, *s = &s1;

    if (avpriv_mpegaudio_decode_header(s, head) != 0) {
        return -1;
    }

    switch(s->layer) {
    case 1:
        *codec_id = AV_CODEC_ID_MP1;
        *frame_size = 384;
        break;
    case 2:
        *codec_id = AV_CODEC_ID_MP2;
        *frame_size = 1152;
        break;
    default:
    case 3:
        if (*codec_id != AV_CODEC_ID_MP3ADU)
            *codec_id = AV_CODEC_ID_MP3;
        if (s->lsf)
            *frame_size = 576;
        else
            *frame_size = 1152;
        break;
    }

    *sample_rate = s->sample_rate;
    *channels = s->nb_channels;
    *bit_rate = s->bit_rate;
    return s->frame_size;
}

int avpriv_ealayer3_decode_header(MPADecodeHeader *s, int header)
{
    int mode_ext = header & 3;
    int mode = (header >> 2) & 3;
    int sfreq_idx = (header >> 4) & 3;
    int version = (header >> 6) & 3;
    uint32_t mpeg = (0x7FF << 21) | (version << 19) | (1 << 17) | (sfreq_idx << 10) | (mode << 6) | (mode_ext << 4);
    return avpriv_mpegaudio_decode_header(s, mpeg);
}
