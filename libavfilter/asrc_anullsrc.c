/*
 * Copyright 2010 S.N. Hemanth Meenakshisundaram <smeenaks ucsd edu>
 * Copyright 2010 Stefano Sabatini <stefano.sabatini-lala poste it>
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
 * null audio source
 */

#include <inttypes.h>
#include <stdio.h>

#include "libavutil/channel_layout.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"

typedef struct ANullContext {
    const AVClass *class;
    AVChannelLayout ch_layout;
    int     sample_rate;
    int64_t duration;
    int nb_samples;             ///< number of samples per requested frame
    int64_t pts;
} ANullContext;

#define OFFSET(x) offsetof(ANullContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption anullsrc_options[]= {
    { "channel_layout", "set channel_layout", OFFSET(ch_layout), AV_OPT_TYPE_CHLAYOUT, {.str = "stereo"}, 0, 0, FLAGS },
    { "cl",             "set channel_layout", OFFSET(ch_layout), AV_OPT_TYPE_CHLAYOUT, {.str = "stereo"}, 0, 0, FLAGS },
    { "sample_rate",    "set sample rate",    OFFSET(sample_rate)   , AV_OPT_TYPE_INT, {.i64 = 44100}, 1, INT_MAX, FLAGS },
    { "r",              "set sample rate",    OFFSET(sample_rate)   , AV_OPT_TYPE_INT, {.i64 = 44100}, 1, INT_MAX, FLAGS },
    { "nb_samples",     "set the number of samples per requested frame", OFFSET(nb_samples), AV_OPT_TYPE_INT, {.i64 = 1024}, 1, UINT16_MAX, FLAGS },
    { "n",              "set the number of samples per requested frame", OFFSET(nb_samples), AV_OPT_TYPE_INT, {.i64 = 1024}, 1, UINT16_MAX, FLAGS },
    { "duration",       "set the audio duration",                        OFFSET(duration),   AV_OPT_TYPE_DURATION, {.i64 = -1}, -1, INT64_MAX, FLAGS },
    { "d",              "set the audio duration",                        OFFSET(duration),   AV_OPT_TYPE_DURATION, {.i64 = -1}, -1, INT64_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(anullsrc);

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const ANullContext *null = ctx->priv;
    const AVChannelLayout chlayouts[] = { null->ch_layout, { 0 } };
    int sample_rates[] = { null->sample_rate, -1 };
    int ret;

    ret = ff_set_common_samplerates_from_list2(ctx, cfg_in, cfg_out, sample_rates);
    if (ret < 0)
        return ret;

    return ff_set_common_channel_layouts_from_list2(ctx, cfg_in, cfg_out, chlayouts);
}

static av_cold int config_props(AVFilterLink *outlink)
{
    ANullContext *null = outlink->src->priv;

    if (null->duration >= 0)
        null->duration = av_rescale(null->duration, null->sample_rate, AV_TIME_BASE);

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    ANullContext *null = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    if (null->duration >= 0 && null->pts >= null->duration) {
        ff_outlink_set_status(outlink, AVERROR_EOF, null->pts);
        return 0;
    }

    if (ff_outlink_frame_wanted(outlink)) {
        AVFrame *frame = ff_get_audio_buffer(outlink, null->duration >= 0 ? FFMIN(null->nb_samples, null->duration - null->pts) : null->nb_samples);

        if (!frame)
            return AVERROR(ENOMEM);

        av_samples_set_silence(frame->extended_data, 0,
                               frame->nb_samples,
                               outlink->ch_layout.nb_channels,
                               frame->format);

        frame->pts = null->pts;
        null->pts += frame->nb_samples;

        return ff_filter_frame(outlink, frame);
    }

    return FFERROR_NOT_READY;
}

static const AVFilterPad outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_props,
    },
};

const FFFilter ff_asrc_anullsrc = {
    .p.name          = "anullsrc",
    .p.description   = NULL_IF_CONFIG_SMALL("Null audio source, return empty audio frames."),
    .p.priv_class    = &anullsrc_class,
    .p.inputs        = NULL,
    .priv_size     = sizeof(ANullContext),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC2(query_formats),
    .activate      = activate,
};
