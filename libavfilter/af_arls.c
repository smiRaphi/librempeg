/*
 * Copyright (c) 2023 Paul B Mahol
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

#include "libavutil/common.h"
#include "libavutil/float_dsp.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "audio.h"
#include "avfilter.h"
#include "formats.h"
#include "filters.h"

enum OutModes {
    IN_MODE,
    DESIRED_MODE,
    OUT_MODE,
    NOISE_MODE,
    ERROR_MODE,
    NB_OMODES
};

typedef struct AudioRLSContext {
    const AVClass *class;

    int order;
    float lambda;
    float delta;
    int output_mode;
    int precision;

    int kernel_size;
    AVFrame *offset;
    AVFrame *delay;
    AVFrame *coeffs;
    AVFrame *p, *dp;
    AVFrame *gains;
    AVFrame *u, *tmp;

    AVFrame *frame[2];

    int (*filter_channels)(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs);

    AVFloatDSPContext *fdsp;
} AudioRLSContext;

#define OFFSET(x) offsetof(AudioRLSContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define AT AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption arls_options[] = {
    { "order",    "set the filter order",  OFFSET(order),  AV_OPT_TYPE_INT,   {.i64=16}, 1, INT16_MAX, A },
    { "lambda",   "set the filter lambda", OFFSET(lambda), AV_OPT_TYPE_FLOAT, {.dbl=1.f}, 0, 1, AT },
    { "delta",    "set the filter delta",  OFFSET(delta),  AV_OPT_TYPE_FLOAT, {.dbl=2.f}, 0, INT16_MAX, A },
    { "out_mode", "set output mode",       OFFSET(output_mode), AV_OPT_TYPE_INT, {.i64=OUT_MODE}, 0, NB_OMODES-1, AT, .unit = "mode" },
    {  "i", "input",   0, AV_OPT_TYPE_CONST, {.i64=IN_MODE},      0, 0, AT, .unit = "mode" },
    {  "d", "desired", 0, AV_OPT_TYPE_CONST, {.i64=DESIRED_MODE}, 0, 0, AT, .unit = "mode" },
    {  "o", "output",  0, AV_OPT_TYPE_CONST, {.i64=OUT_MODE},     0, 0, AT, .unit = "mode" },
    {  "n", "noise",   0, AV_OPT_TYPE_CONST, {.i64=NOISE_MODE},   0, 0, AT, .unit = "mode" },
    {  "e", "error",   0, AV_OPT_TYPE_CONST, {.i64=ERROR_MODE},   0, 0, AT, .unit = "mode" },
    { "precision", "set processing precision", OFFSET(precision),  AV_OPT_TYPE_INT,    {.i64=0},   0, 2, A, .unit = "precision" },
    {   "auto",  "set auto processing precision",                  0, AV_OPT_TYPE_CONST, {.i64=0}, 0, 0, A, .unit = "precision" },
    {   "float", "set single-floating point processing precision", 0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, A, .unit = "precision" },
    {   "double","set double-floating point processing precision", 0, AV_OPT_TYPE_CONST, {.i64=2}, 0, 0, A, .unit = "precision" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(arls);

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const AudioRLSContext *s = ctx->priv;
    static const enum AVSampleFormat sample_fmts[3][3] = {
        { AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_DBLP, AV_SAMPLE_FMT_NONE },
        { AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE },
        { AV_SAMPLE_FMT_DBLP, AV_SAMPLE_FMT_NONE },
    };
    int ret;

    if ((ret = ff_set_common_formats_from_list2(ctx, cfg_in, cfg_out,
                                                sample_fmts[s->precision])) < 0)
        return ret;

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    AudioRLSContext *s = ctx->priv;

    FF_FILTER_FORWARD_STATUS_BACK_ALL(ctx->outputs[0], ctx);

    if (!s->frame[0]) {
        int ret = ff_inlink_consume_frame(ctx->inputs[0], &s->frame[0]);
        if (ret < 0)
            return ret;
    }

    if (s->frame[0] && !s->frame[1]) {
        int ret = ff_inlink_consume_samples(ctx->inputs[1],
                                            s->frame[0]->nb_samples,
                                            s->frame[0]->nb_samples,
                                            &s->frame[1]);
        if (ret < 0)
            return ret;
    }

    if (s->frame[0] && s->frame[1]) {
        AVFrame *out;

        out = ff_get_audio_buffer(ctx->outputs[0], s->frame[0]->nb_samples);
        if (!out) {
            av_frame_free(&s->frame[0]);
            av_frame_free(&s->frame[1]);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, s->frame[0]);

        ff_filter_execute(ctx, s->filter_channels, out, NULL,
                          FFMIN(ctx->outputs[0]->ch_layout.nb_channels, ff_filter_get_nb_threads(ctx)));


        av_frame_free(&s->frame[0]);
        av_frame_free(&s->frame[1]);

        return ff_filter_frame(ctx->outputs[0], out);
    }

    for (int i = 0; i < 2; i++) {
        int64_t pts;
        int status;

        if (ff_inlink_acknowledge_status(ctx->inputs[i], &status, &pts)) {
            ff_outlink_set_status(ctx->outputs[0], status, pts);
            return 0;
        }
    }

    if (ff_outlink_frame_wanted(ctx->outputs[0])) {
        for (int i = 0; i < 2; i++) {
            if (s->frame[i])
                continue;
            ff_inlink_request_frame(ctx->inputs[i]);
            return 0;
        }
    }
    return 0;
}

#define DEPTH 32
#include "arls_template.c"

#undef DEPTH
#define DEPTH 64
#include "arls_template.c"

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioRLSContext *s = ctx->priv;

    s->kernel_size = FFALIGN(s->order, 16);

    if (!s->offset)
        s->offset = ff_get_audio_buffer(outlink, 1);
    if (!s->delay)
        s->delay = ff_get_audio_buffer(outlink, 2 * s->kernel_size);
    if (!s->coeffs)
        s->coeffs = ff_get_audio_buffer(outlink, 2 * s->kernel_size);
    if (!s->gains)
        s->gains = ff_get_audio_buffer(outlink, s->kernel_size);
    if (!s->p)
        s->p = ff_get_audio_buffer(outlink, s->kernel_size * s->kernel_size);
    if (!s->dp)
        s->dp = ff_get_audio_buffer(outlink, s->kernel_size * s->kernel_size);
    if (!s->u)
        s->u = ff_get_audio_buffer(outlink, s->kernel_size);
    if (!s->tmp)
        s->tmp = ff_get_audio_buffer(outlink, s->kernel_size);

    if (!s->delay || !s->coeffs || !s->p || !s->dp || !s->gains || !s->offset || !s->u || !s->tmp)
        return AVERROR(ENOMEM);

    for (int ch = 0; ch < s->offset->ch_layout.nb_channels; ch++) {
        int *dst = (int *)s->offset->extended_data[ch];

        for (int i = 0; i < s->kernel_size; i++)
            dst[0] = s->kernel_size - 1;
    }

    switch (outlink->format) {
    case AV_SAMPLE_FMT_DBLP:
        for (int ch = 0; ch < s->p->ch_layout.nb_channels; ch++) {
            double *dst = (double *)s->p->extended_data[ch];

            for (int i = 0; i < s->kernel_size; i++)
                dst[i * s->kernel_size + i] = s->delta;
        }

        s->filter_channels = filter_channels_double;
        break;
    case AV_SAMPLE_FMT_FLTP:
        for (int ch = 0; ch < s->p->ch_layout.nb_channels; ch++) {
            float *dst = (float *)s->p->extended_data[ch];

            for (int i = 0; i < s->kernel_size; i++)
                dst[i * s->kernel_size + i] = s->delta;
        }

        s->filter_channels = filter_channels_float;
        break;
    default:
        return AVERROR_BUG;
    }

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    AudioRLSContext *s = ctx->priv;

    s->fdsp = avpriv_float_dsp_alloc(0);
    if (!s->fdsp)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioRLSContext *s = ctx->priv;

    av_freep(&s->fdsp);
    av_frame_free(&s->delay);
    av_frame_free(&s->coeffs);
    av_frame_free(&s->gains);
    av_frame_free(&s->offset);
    av_frame_free(&s->p);
    av_frame_free(&s->dp);
    av_frame_free(&s->u);
    av_frame_free(&s->tmp);
    av_frame_free(&s->frame[0]);
    av_frame_free(&s->frame[1]);
}

static const AVFilterPad inputs[] = {
    {
        .name = "input",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    {
        .name = "desired",
        .type = AVMEDIA_TYPE_AUDIO,
    },
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_output,
    },
};

const FFFilter ff_af_arls = {
    .p.name         = "arls",
    .p.description  = NULL_IF_CONFIG_SMALL("Apply Recursive Least Squares algorithm to first audio stream."),
    .p.priv_class   = &arls_class,
    .p.flags        = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                      AVFILTER_FLAG_SLICE_THREADS,
    .priv_size      = sizeof(AudioRLSContext),
    .init           = init,
    .uninit         = uninit,
    .activate       = activate,
    FILTER_INPUTS(inputs),
    FILTER_OUTPUTS(outputs),
    FILTER_QUERY_FUNC2(query_formats),
    .process_command = ff_filter_process_command,
};
