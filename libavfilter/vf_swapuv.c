/*
 * Copyright (c) 2002 Michael Niedermayer <michaelni@gmx.at>
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
 * swap UV filter
 */

#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

static void do_swap(AVFrame *frame)
{
    FFSWAP(uint8_t*,     frame->data[1],     frame->data[2]);
    FFSWAP(int,          frame->linesize[1], frame->linesize[2]);
    FFSWAP(AVBufferRef*, frame->buf[1],      frame->buf[2]);
}

static AVFrame *get_video_buffer(AVFilterLink *link, int w, int h)
{
    AVFrame *picref = ff_default_get_video_buffer(link, w, h);
    do_swap(picref);
    return picref;
}

static int filter_frame(AVFilterLink *link, AVFrame *inpicref)
{
    do_swap(inpicref);
    return ff_filter_frame(link->dst->outputs[0], inpicref);
}

static int is_planar_yuv(const AVPixFmtDescriptor *desc)
{
    if (desc->flags & ~(AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PLANAR | AV_PIX_FMT_FLAG_ALPHA) ||
        desc->nb_components < 3 ||
        (desc->comp[1].depth != desc->comp[2].depth))
        return 0;
    for (int i = 0; i < desc->nb_components; i++) {
        if (desc->comp[i].offset != 0 ||
            desc->comp[i].shift != 0 ||
            desc->comp[i].plane != i)
            return 0;
    }

    return 1;
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    AVFilterFormats *formats = NULL;
    int ret;

    for (int fmt = 0; av_pix_fmt_desc_get(fmt); fmt++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
        if (is_planar_yuv(desc) && (ret = ff_add_format(&formats, fmt)) < 0)
            return ret;
    }

    return ff_set_common_formats2(ctx, cfg_in, cfg_out, formats);
}

static const AVFilterPad swapuv_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .get_buffer.video = get_video_buffer,
        .filter_frame     = filter_frame,
    },
};

const FFFilter ff_vf_swapuv = {
    .p.name        = "swapuv",
    .p.description = NULL_IF_CONFIG_SMALL("Swap U and V components."),
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    FILTER_INPUTS(swapuv_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
};
