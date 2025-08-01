/*
 * Copyright (C) 2012 Michael Niedermayer <michaelni@gmx.at>
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

#include <float.h> /* FLT_MAX */

#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "filters.h"
#include "vf_idet.h"

#define OFFSET(x) offsetof(IDETContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption idet_options[] = {
    { "intl_thres", "set interlacing threshold", OFFSET(interlace_threshold),   AV_OPT_TYPE_FLOAT, {.dbl = 1.04}, -1, FLT_MAX, FLAGS },
    { "prog_thres", "set progressive threshold", OFFSET(progressive_threshold), AV_OPT_TYPE_FLOAT, {.dbl = 1.5},  -1, FLT_MAX, FLAGS },
    { "rep_thres",  "set repeat threshold",      OFFSET(repeat_threshold),      AV_OPT_TYPE_FLOAT, {.dbl = 3.0},  -1, FLT_MAX, FLAGS },
    { "half_life", "half life of cumulative statistics", OFFSET(half_life),     AV_OPT_TYPE_FLOAT, {.dbl = 0.0},  -1, INT_MAX, FLAGS },
    { "analyze_interlaced_flag", "set number of frames to use to determine if the interlace flag is accurate", OFFSET(analyze_interlaced_flag), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, INT_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(idet);

static const char *type2str(Type type)
{
    switch(type) {
        case TFF          : return "tff";
        case BFF          : return "bff";
        case PROGRESSIVE  : return "progressive";
        case UNDETERMINED : return "undetermined";
    }
    return NULL;
}

#define PRECISION 1048576

static uint64_t uintpow(uint64_t b,unsigned int e)
{
    uint64_t r=1;
    while(e--) r*=b;
    return r;
}

static int av_dict_set_fxp(AVDictionary **pm, const char *key, uint64_t value, unsigned int digits,
                int flags)
{
    char valuestr[44];
    uint64_t print_precision = uintpow(10, digits);

    value = av_rescale(value, print_precision, PRECISION);

    snprintf(valuestr, sizeof(valuestr), "%"PRId64".%0*"PRId64,
             value / print_precision, digits, value % print_precision);

    return av_dict_set(pm, key, valuestr, flags);
}

static const char *rep2str(RepeatedField repeated_field)
{
    switch(repeated_field) {
        case REPEAT_NONE    : return "neither";
        case REPEAT_TOP     : return "top";
        case REPEAT_BOTTOM  : return "bottom";
    }
    return NULL;
}

int ff_idet_filter_line_c(const uint8_t *a, const uint8_t *b, const uint8_t *c, int w)
{
    int x;
    int ret=0;

    for(x=0; x<w; x++){
        int v = (*a++ + *c++) - 2 * *b++;
        ret += FFABS(v);
    }

    return ret;
}

int ff_idet_filter_line_c_16bit(const uint16_t *a, const uint16_t *b, const uint16_t *c, int w)
{
    int x;
    int ret=0;

    for(x=0; x<w; x++){
        int v = (*a++ + *c++) - 2 * *b++;
        ret += FFABS(v);
    }

    return ret;
}

static void filter(AVFilterContext *ctx)
{
    IDETContext *idet = ctx->priv;
    int y, i;
    int64_t alpha[2]={0};
    int64_t delta=0;
    int64_t gamma[2]={0};
    Type type, best_type;
    RepeatedField repeat;
    int match = 0;
    AVDictionary **metadata = &idet->cur->metadata;

    for (i = 0; i < idet->csp->nb_components; i++) {
        int w = idet->cur->width;
        int h = idet->cur->height;
        int refs = idet->cur->linesize[i];

        if (i && i<3) {
            w = AV_CEIL_RSHIFT(w, idet->csp->log2_chroma_w);
            h = AV_CEIL_RSHIFT(h, idet->csp->log2_chroma_h);
        }

        for (y = 2; y < h - 2; y++) {
            uint8_t *prev = &idet->prev->data[i][y*refs];
            uint8_t *cur  = &idet->cur ->data[i][y*refs];
            uint8_t *next = &idet->next->data[i][y*refs];
            alpha[ y   &1] += idet->filter_line(cur-refs, prev, cur+refs, w);
            alpha[(y^1)&1] += idet->filter_line(cur-refs, next, cur+refs, w);
            delta          += idet->filter_line(cur-refs,  cur, cur+refs, w);
            gamma[(y^1)&1] += idet->filter_line(cur     , prev, cur     , w);
        }
    }

    if      (alpha[0] > idet->interlace_threshold * alpha[1]){
        type = TFF;
    }else if(alpha[1] > idet->interlace_threshold * alpha[0]){
        type = BFF;
    }else if(alpha[1] > idet->progressive_threshold * delta){
        type = PROGRESSIVE;
    }else{
        type = UNDETERMINED;
    }

    if ( gamma[0] > idet->repeat_threshold * gamma[1] ){
        repeat = REPEAT_TOP;
    } else if ( gamma[1] > idet->repeat_threshold * gamma[0] ){
        repeat = REPEAT_BOTTOM;
    } else {
        repeat = REPEAT_NONE;
    }

    memmove(idet->history+1, idet->history, HIST_SIZE-1);
    idet->history[0] = type;
    best_type = UNDETERMINED;
    for(i=0; i<HIST_SIZE; i++){
        if(idet->history[i] != UNDETERMINED){
            if(best_type == UNDETERMINED)
                best_type = idet->history[i];

            if(idet->history[i] == best_type) {
                match++;
            }else{
                match=0;
                break;
            }
        }
    }
    if(idet->last_type == UNDETERMINED){
        if(match  ) idet->last_type = best_type;
    }else{
        if(match>2) idet->last_type = best_type;
    }

    if      (idet->last_type == TFF){
        idet->cur->flags |= (AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
    }else if(idet->last_type == BFF){
        idet->cur->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
        idet->cur->flags |= AV_FRAME_FLAG_INTERLACED;
    }else if(idet->last_type == PROGRESSIVE){
        idet->cur->flags &= ~AV_FRAME_FLAG_INTERLACED;
    }

    for(i=0; i<3; i++)
        idet->repeats[i]  = av_rescale(idet->repeats [i], idet->decay_coefficient, PRECISION);

    for(i=0; i<4; i++){
        idet->prestat [i] = av_rescale(idet->prestat [i], idet->decay_coefficient, PRECISION);
        idet->poststat[i] = av_rescale(idet->poststat[i], idet->decay_coefficient, PRECISION);
    }

    idet->total_repeats [         repeat] ++;
    idet->repeats       [         repeat] += PRECISION;

    idet->total_prestat [           type] ++;
    idet->prestat       [           type] += PRECISION;

    idet->total_poststat[idet->last_type] ++;
    idet->poststat      [idet->last_type] += PRECISION;

    av_log(ctx, AV_LOG_DEBUG, "Repeated Field:%12s, Single frame:%12s, Multi frame:%12s\n",
           rep2str(repeat), type2str(type), type2str(idet->last_type));

    av_dict_set    (metadata, "lavfi.idet.repeated.current_frame", rep2str(repeat), 0);
    av_dict_set_fxp(metadata, "lavfi.idet.repeated.neither",       idet->repeats[REPEAT_NONE], 2, 0);
    av_dict_set_fxp(metadata, "lavfi.idet.repeated.top",           idet->repeats[REPEAT_TOP], 2, 0);
    av_dict_set_fxp(metadata, "lavfi.idet.repeated.bottom",        idet->repeats[REPEAT_BOTTOM], 2, 0);

    av_dict_set    (metadata, "lavfi.idet.single.current_frame",   type2str(type), 0);
    av_dict_set_fxp(metadata, "lavfi.idet.single.tff",             idet->prestat[TFF], 2 , 0);
    av_dict_set_fxp(metadata, "lavfi.idet.single.bff",             idet->prestat[BFF], 2, 0);
    av_dict_set_fxp(metadata, "lavfi.idet.single.progressive",     idet->prestat[PROGRESSIVE], 2, 0);
    av_dict_set_fxp(metadata, "lavfi.idet.single.undetermined",    idet->prestat[UNDETERMINED], 2, 0);

    av_dict_set    (metadata, "lavfi.idet.multiple.current_frame", type2str(idet->last_type), 0);
    av_dict_set_fxp(metadata, "lavfi.idet.multiple.tff",           idet->poststat[TFF], 2, 0);
    av_dict_set_fxp(metadata, "lavfi.idet.multiple.bff",           idet->poststat[BFF], 2, 0);
    av_dict_set_fxp(metadata, "lavfi.idet.multiple.progressive",   idet->poststat[PROGRESSIVE], 2, 0);
    av_dict_set_fxp(metadata, "lavfi.idet.multiple.undetermined",  idet->poststat[UNDETERMINED], 2, 0);
}

static int filter_frame(AVFilterLink *link, AVFrame *picref)
{
    AVFilterContext *ctx = link->dst;
    IDETContext *idet = ctx->priv;

    // initial frame(s) and not interlaced, just pass through for
    // the analyze_interlaced_flag mode
    if (idet->analyze_interlaced_flag &&
        !(picref->flags & AV_FRAME_FLAG_INTERLACED) &&
        !idet->next) {
        return ff_filter_frame(ctx->outputs[0], picref);
    }
    if (idet->analyze_interlaced_flag_done) {
        if ((picref->flags & AV_FRAME_FLAG_INTERLACED) && idet->interlaced_flag_accuracy < 0) {
            picref->flags &= ~AV_FRAME_FLAG_INTERLACED;
        }
        return ff_filter_frame(ctx->outputs[0], picref);
    }

    av_frame_free(&idet->prev);

    if(   picref->width  != link->w
       || picref->height != link->h
       || picref->format != link->format) {
        link->dst->inputs[0]->format = picref->format;
        link->dst->inputs[0]->w      = picref->width;
        link->dst->inputs[0]->h      = picref->height;

        av_frame_free(&idet->cur );
        av_frame_free(&idet->next);
    }

    idet->prev = idet->cur;
    idet->cur  = idet->next;
    idet->next = picref;

    if (!idet->cur &&
        !(idet->cur = av_frame_clone(idet->next)))
        return AVERROR(ENOMEM);

    if (!idet->prev)
        return 0;

    if (!idet->csp)
        idet->csp = av_pix_fmt_desc_get(link->format);
    if (idet->csp->comp[0].depth > 8){
        idet->filter_line = (ff_idet_filter_func)ff_idet_filter_line_c_16bit;
#if ARCH_X86
        ff_idet_init_x86(idet, 1);
#endif
    }

    if (idet->analyze_interlaced_flag) {
        if (idet->cur->flags & AV_FRAME_FLAG_INTERLACED) {
            idet->cur->flags &= ~AV_FRAME_FLAG_INTERLACED;
            filter(ctx);
            if (idet->last_type == PROGRESSIVE) {
                idet->interlaced_flag_accuracy --;
                idet->analyze_interlaced_flag --;
            } else if (idet->last_type != UNDETERMINED) {
                idet->interlaced_flag_accuracy ++;
                idet->analyze_interlaced_flag --;
            }
            if (idet->analyze_interlaced_flag == 1) {
                ff_filter_frame(ctx->outputs[0], av_frame_clone(idet->cur));

                if ((idet->next->flags & AV_FRAME_FLAG_INTERLACED) && idet->interlaced_flag_accuracy < 0) {
                    idet->next->flags &= ~AV_FRAME_FLAG_INTERLACED;
                }
                idet->analyze_interlaced_flag_done = 1;
                av_log(ctx, AV_LOG_INFO, "Final flag accuracy %d\n", idet->interlaced_flag_accuracy);
                return ff_filter_frame(ctx->outputs[0], av_frame_clone(idet->next));
            }
        }
    } else {
        filter(ctx);
    }

    return ff_filter_frame(ctx->outputs[0], av_frame_clone(idet->cur));
}

static int request_frame(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    IDETContext *idet = ctx->priv;
    int ret;

    if (idet->eof)
        return AVERROR_EOF;

    ret = ff_request_frame(link->src->inputs[0]);

    if (ret == AVERROR_EOF && idet->cur && !idet->analyze_interlaced_flag_done) {
        AVFrame *next = av_frame_clone(idet->next);

        if (!next)
            return AVERROR(ENOMEM);

        ret = filter_frame(link->src->inputs[0], next);
        idet->eof = 1;
    }

    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    IDETContext *idet = ctx->priv;

    av_log(ctx, AV_LOG_INFO, "Repeated Fields: Neither:%6"PRId64" Top:%6"PRId64" Bottom:%6"PRId64"\n",
           idet->total_repeats[REPEAT_NONE],
           idet->total_repeats[REPEAT_TOP],
           idet->total_repeats[REPEAT_BOTTOM]
        );
    av_log(ctx, AV_LOG_INFO, "Single frame detection: TFF:%6"PRId64" BFF:%6"PRId64" Progressive:%6"PRId64" Undetermined:%6"PRId64"\n",
           idet->total_prestat[TFF],
           idet->total_prestat[BFF],
           idet->total_prestat[PROGRESSIVE],
           idet->total_prestat[UNDETERMINED]
        );
    av_log(ctx, AV_LOG_INFO, "Multi frame detection: TFF:%6"PRId64" BFF:%6"PRId64" Progressive:%6"PRId64" Undetermined:%6"PRId64"\n",
           idet->total_poststat[TFF],
           idet->total_poststat[BFF],
           idet->total_poststat[PROGRESSIVE],
           idet->total_poststat[UNDETERMINED]
        );

    av_frame_free(&idet->prev);
    av_frame_free(&idet->cur );
    av_frame_free(&idet->next);
}

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUV411P,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_YUV420P9,
    AV_PIX_FMT_YUV422P9,
    AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUV422P10,
    AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12,
    AV_PIX_FMT_YUV422P12,
    AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_YUV420P14,
    AV_PIX_FMT_YUV422P14,
    AV_PIX_FMT_YUV444P14,
    AV_PIX_FMT_YUV420P16,
    AV_PIX_FMT_YUV422P16,
    AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_YUVA422P,
    AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_NONE
};

static av_cold int init(AVFilterContext *ctx)
{
    IDETContext *idet = ctx->priv;

    idet->eof = 0;
    idet->last_type = UNDETERMINED;
    memset(idet->history, UNDETERMINED, HIST_SIZE);

    if( idet->half_life > 0 )
        idet->decay_coefficient = lrint( PRECISION * exp2(-1.0 / idet->half_life) );
    else
        idet->decay_coefficient = PRECISION;

    idet->filter_line = ff_idet_filter_line_c;

#if ARCH_X86
    ff_idet_init_x86(idet, 0);
#endif

    return 0;
}

static const AVFilterPad idet_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad idet_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame
    },
};

const FFFilter ff_vf_idet = {
    .p.name        = "idet",
    .p.description = NULL_IF_CONFIG_SMALL("Interlace detect Filter."),
    .p.flags       = AVFILTER_FLAG_METADATA_ONLY,
    .p.priv_class  = &idet_class,
    .priv_size     = sizeof(IDETContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(idet_inputs),
    FILTER_OUTPUTS(idet_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
};
