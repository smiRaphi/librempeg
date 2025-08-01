/*
 * Copyright (c) 2018 Marton Balint
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

#include "config_components.h"

#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"

typedef struct CueContext {
    const AVClass *class;
    int64_t first_pts;
    int64_t cue;
    int64_t preroll;
    int64_t buffer;
    int status;
} CueContext;

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    CueContext *s = ctx->priv;

    FF_FILTER_FORWARD_STATUS_BACK(outlink, inlink);

    if (ff_inlink_queued_frames(inlink)) {
        AVFrame *frame = ff_inlink_peek_frame(inlink, 0);
        int64_t pts = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);

        if (!s->status) {
            s->first_pts = pts;
            s->status++;
        }
        if (s->status == 1) {
            if (pts - s->first_pts < s->preroll) {
                int ret = ff_inlink_consume_frame(inlink, &frame);
                if (ret < 0)
                    return ret;
                return ff_filter_frame(outlink, frame);
            }
            s->first_pts = pts;
            s->status++;
        }
        if (s->status == 2) {
            frame = ff_inlink_peek_frame(inlink, ff_inlink_queued_frames(inlink) - 1);
            pts = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);
            if (!(pts - s->first_pts < s->buffer && (av_gettime() - s->cue) < 0))
                s->status++;
        }
        if (s->status == 3) {
            int64_t diff;
            while ((diff = (av_gettime() - s->cue)) < 0)
                av_usleep(av_clip(-diff / 2, 100, 1000000));
            s->status++;
        }
        if (s->status == 4) {
            int ret = ff_inlink_consume_frame(inlink, &frame);
            if (ret < 0)
                return ret;
            return ff_filter_frame(outlink, frame);
        }
    }

    FF_FILTER_FORWARD_STATUS(inlink, outlink);
    FF_FILTER_FORWARD_WANTED(outlink, inlink);

    return FFERROR_NOT_READY;
}

#define OFFSET(x) offsetof(CueContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
static const AVOption options[] = {
    { "cue", "cue unix timestamp in microseconds", OFFSET(cue), AV_OPT_TYPE_INT64, { .i64 = 0 }, 0, INT64_MAX, FLAGS },
    { "preroll", "preroll duration in seconds", OFFSET(preroll), AV_OPT_TYPE_DURATION, { .i64 = 0 }, 0, INT64_MAX, FLAGS },
    { "buffer", "buffer duration in seconds", OFFSET(buffer), AV_OPT_TYPE_DURATION, { .i64 = 0 }, 0, INT64_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS_EXT(cue_acue, "(a)cue", options);

#if CONFIG_CUE_FILTER
const FFFilter ff_vf_cue = {
    .p.name        = "cue",
    .p.description = NULL_IF_CONFIG_SMALL("Delay filtering to match a cue."),
    .p.priv_class  = &cue_acue_class,
    .priv_size   = sizeof(CueContext),
    FILTER_INPUTS(ff_video_default_filterpad),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    .activate    = activate,
};
#endif /* CONFIG_CUE_FILTER */

#if CONFIG_ACUE_FILTER
const FFFilter ff_af_acue = {
    .p.name        = "acue",
    .p.description = NULL_IF_CONFIG_SMALL("Delay filtering to match a cue."),
    .p.priv_class  = &cue_acue_class,
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .priv_size   = sizeof(CueContext),
    FILTER_INPUTS(ff_audio_default_filterpad),
    FILTER_OUTPUTS(ff_audio_default_filterpad),
    .activate    = activate,
};
#endif /* CONFIG_ACUE_FILTER */
