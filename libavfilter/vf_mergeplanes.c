/*
 * Copyright (c) 2013 Paul B Mahol
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

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "framesync.h"
#include "video.h"

typedef struct InputParam {
    int depth[4];
    int nb_planes;
    int planewidth[4];
    int planeheight[4];
} InputParam;

typedef struct MergePlanesContext {
    const AVClass *class;
    const enum AVPixelFormat out_fmt;

    int *input_map;
    unsigned nb_input_map;
    int *plane_map;
    unsigned nb_plane_map;

    int nb_inputs;
    int nb_planes;
    int planewidth[4];
    int planeheight[4];
    const AVPixFmtDescriptor *indesc[4];
    const AVPixFmtDescriptor *outdesc;

    FFFrameSync fs;
} MergePlanesContext;

#define OFFSET(x) offsetof(MergePlanesContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define AR AV_OPT_TYPE_FLAG_ARRAY
static const AVOptionArrayDef def_map = {.def="0",.size_min=1,.size_max=4,.sep=' '};

static const AVOption mergeplanes_options[] = {
    { "format", "set output pixel format", OFFSET(out_fmt), AV_OPT_TYPE_PIXEL_FMT, {.i64=AV_PIX_FMT_YUVA444P}, 0, INT_MAX, .flags=FLAGS },
    { "input", "set input stream to output stream mapping", OFFSET(input_map), AV_OPT_TYPE_INT|AR, {.arr=&def_map}, 0, 3, FLAGS },
    { "plane", "set input plane to output plane mapping",   OFFSET(plane_map), AV_OPT_TYPE_INT|AR, {.arr=&def_map}, 0, 3, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(mergeplanes);

static av_cold int init(AVFilterContext *ctx)
{
    MergePlanesContext *s = ctx->priv;
    int ret;

    s->outdesc = av_pix_fmt_desc_get(s->out_fmt);
    if (!(s->outdesc->flags & AV_PIX_FMT_FLAG_PLANAR) ||
        s->outdesc->nb_components < 2) {
        av_log(ctx, AV_LOG_ERROR, "Only planar formats with more than one component are supported.\n");
        return AVERROR(EINVAL);
    }
    s->nb_planes = av_pix_fmt_count_planes(s->out_fmt);

    for (int i = 0; i < s->nb_input_map; i++)
        s->nb_inputs = FFMAX(s->nb_inputs, s->input_map[i]+1);

    av_assert0(s->nb_inputs && s->nb_inputs <= 4);

    for (int i = 0; i < s->nb_inputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_VIDEO;
        pad.name = av_asprintf("in%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_append_inpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    return 0;
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const MergePlanesContext *s = ctx->priv;
    AVFilterFormats *formats = NULL;
    int ret;

    for (int i = 0; av_pix_fmt_desc_get(i); i++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(i);
        if (desc->comp[0].depth == s->outdesc->comp[0].depth &&
            (desc->comp[0].depth <= 8 || (desc->flags & AV_PIX_FMT_FLAG_BE) == (s->outdesc->flags & AV_PIX_FMT_FLAG_BE)) &&
            av_pix_fmt_count_planes(i) == desc->nb_components &&
            (ret = ff_add_format(&formats, i)) < 0)
                return ret;
    }

    for (int i = 0; i < s->nb_inputs; i++)
        if ((ret = ff_formats_ref(formats, &cfg_in[i]->formats)) < 0)
            return ret;

    formats = NULL;
    if ((ret = ff_add_format(&formats, s->out_fmt)) < 0 ||
        (ret = ff_formats_ref(formats, &cfg_out[0]->formats)) < 0)
        return ret;

    return 0;
}

static int process_frame(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFilterLink *outlink = ctx->outputs[0];
    MergePlanesContext *s = fs->opaque;
    AVFrame *in[4] = { NULL };
    AVFrame *out;
    int ret;

    for (int i = 0; i < s->nb_inputs; i++) {
        if ((ret = ff_framesync_get_frame(&s->fs, i, &in[i], 0)) < 0)
            return ret;
    }

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);
    out->pts = av_rescale_q(s->fs.pts, s->fs.time_base, outlink->time_base);

    for (int i = 0; i < s->nb_planes; i++) {
        const int input = s->input_map[FFMIN(i, s->nb_input_map-1)];
        const int plane = s->plane_map[FFMIN(i, s->nb_plane_map-1)];

        av_image_copy_plane(out->data[i], out->linesize[i],
                            in[input]->data[plane], in[input]->linesize[plane],
                            s->planewidth[i] * ((s->indesc[input]->comp[plane].depth + 7) / 8),
                            s->planeheight[i]);
    }

    return ff_filter_frame(outlink, out);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    MergePlanesContext *s = ctx->priv;
    FilterLink *il = ff_filter_link(ctx->inputs[0]);
    FilterLink *ol = ff_filter_link(outlink);
    InputParam inputsp[4];
    FFFrameSyncIn *in;
    int ret;

    if ((ret = ff_framesync_init(&s->fs, ctx, s->nb_inputs)) < 0)
        return ret;

    in = s->fs.in;
    s->fs.opaque = s;
    s->fs.on_event = process_frame;

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;
    outlink->time_base = ctx->inputs[0]->time_base;
    ol->frame_rate = il->frame_rate;
    outlink->sample_aspect_ratio = ctx->inputs[0]->sample_aspect_ratio;

    s->planewidth[1]  =
    s->planewidth[2]  = AV_CEIL_RSHIFT(outlink->w, s->outdesc->log2_chroma_w);
    s->planewidth[0]  =
    s->planewidth[3]  = outlink->w;
    s->planeheight[1] =
    s->planeheight[2] = AV_CEIL_RSHIFT(outlink->h, s->outdesc->log2_chroma_h);
    s->planeheight[0] =
    s->planeheight[3] = outlink->h;

    for (int i = 0; i < s->nb_inputs; i++) {
        InputParam *inputp = &inputsp[i];
        AVFilterLink *inlink = ctx->inputs[i];
        s->indesc[i] = av_pix_fmt_desc_get(inlink->format);

        if (outlink->sample_aspect_ratio.num != inlink->sample_aspect_ratio.num ||
            outlink->sample_aspect_ratio.den != inlink->sample_aspect_ratio.den) {
            av_log(ctx, AV_LOG_WARNING, "input #%d link %s SAR %d:%d "
                                      "does not match output link %s SAR %d:%d\n",
                                      i, ctx->input_pads[i].name,
                                      inlink->sample_aspect_ratio.num,
                                      inlink->sample_aspect_ratio.den,
                                      ctx->output_pads[0].name,
                                      outlink->sample_aspect_ratio.num,
                                      outlink->sample_aspect_ratio.den);
        }

        inputp->planewidth[1]  =
        inputp->planewidth[2]  = AV_CEIL_RSHIFT(inlink->w, s->indesc[i]->log2_chroma_w);
        inputp->planewidth[0]  =
        inputp->planewidth[3]  = inlink->w;
        inputp->planeheight[1] =
        inputp->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, s->indesc[i]->log2_chroma_h);
        inputp->planeheight[0] =
        inputp->planeheight[3] = inlink->h;
        inputp->nb_planes = av_pix_fmt_count_planes(inlink->format);

        for (int j = 0; j < inputp->nb_planes; j++)
            inputp->depth[j] = s->indesc[i]->comp[j].depth;

        in[i].time_base = inlink->time_base;
        in[i].sync   = 1;
        in[i].before = EXT_STOP;
        in[i].after  = EXT_STOP;
    }

    for (int i = 0; i < s->nb_planes; i++) {
        const int input = s->input_map[FFMIN(i, s->nb_input_map-1)];
        const int plane = s->plane_map[FFMIN(i, s->nb_plane_map-1)];
        InputParam *inputp = &inputsp[input];

        if (plane + 1 > inputp->nb_planes) {
            av_log(ctx, AV_LOG_ERROR, "input %d does not have %d plane\n",
                                      input, plane);
            goto fail;
        }
        if (s->outdesc->comp[i].depth != inputp->depth[plane]) {
            av_log(ctx, AV_LOG_ERROR, "output plane %d depth %d does not "
                                      "match input %d plane %d depth %d\n",
                                      i, s->outdesc->comp[i].depth,
                                      input, plane, inputp->depth[plane]);
            goto fail;
        }
        if (s->planewidth[i] != inputp->planewidth[plane]) {
            av_log(ctx, AV_LOG_ERROR, "output plane %d width %d does not "
                                      "match input %d plane %d width %d\n",
                                      i, s->planewidth[i],
                                      input, plane, inputp->planewidth[plane]);
            goto fail;
        }
        if (s->planeheight[i] != inputp->planeheight[plane]) {
            av_log(ctx, AV_LOG_ERROR, "output plane %d height %d does not "
                                      "match input %d plane %d height %d\n",
                                      i, s->planeheight[i],
                                      input, plane, inputp->planeheight[plane]);
            goto fail;
        }
    }

    return ff_framesync_configure(&s->fs);
fail:
    return AVERROR(EINVAL);
}

static int activate(AVFilterContext *ctx)
{
    MergePlanesContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    MergePlanesContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
}

static const AVFilterPad mergeplanes_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const FFFilter ff_vf_mergeplanes = {
    .p.name        = "mergeplanes",
    .p.description = NULL_IF_CONFIG_SMALL("Merge planes."),
    .p.priv_class  = &mergeplanes_class,
    .p.inputs      = NULL,
    .p.flags       = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .priv_size     = sizeof(MergePlanesContext),
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    FILTER_OUTPUTS(mergeplanes_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
