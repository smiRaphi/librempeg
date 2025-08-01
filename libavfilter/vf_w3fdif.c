/*
 * Copyright (C) 2012 British Broadcasting Corporation, All Rights Reserved
 * Author of de-interlace algorithm: Jim Easterbrook for BBC R&D
 * Based on the process described by Martin Weston for BBC R&D
 * Author of FFmpeg filter: Mark Himsley for BBC Broadcast Systems Development
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
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "filters.h"
#include "video.h"
#include "w3fdif.h"

typedef struct W3FDIFContext {
    const AVClass *class;
    int filter;           ///< 0 is simple, 1 is more complex
    int mode;             ///< 0 is frame, 1 is field
    int parity;           ///< frame field parity
    int deint;            ///< which frames to deinterlace
    int linesize[4];      ///< bytes of pixel data per line for each plane
    int planeheight[4];   ///< height of each plane
    int field;            ///< which field are we on, 0 or 1
    int eof;
    int nb_planes;
    AVFrame *prev, *cur, *next;  ///< previous, current, next frames
    int32_t **work_line;  ///< lines we are calculating
    int nb_threads;
    int max;

    W3FDIFDSPContext dsp;
} W3FDIFContext;

#define OFFSET(x) offsetof(W3FDIFContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM
#define CONST(name, help, val, u) { name, help, 0, AV_OPT_TYPE_CONST, {.i64=val}, 0, 0, FLAGS, .unit = u }

static const AVOption w3fdif_options[] = {
    { "filter", "specify the filter", OFFSET(filter), AV_OPT_TYPE_INT, {.i64=1}, 0, 1, FLAGS, .unit = "filter" },
    CONST("simple",  NULL, 0, "filter"),
    CONST("complex", NULL, 1, "filter"),
    { "mode",   "specify the interlacing mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=1}, 0, 1, FLAGS, .unit = "mode"},
    CONST("frame", "send one frame for each frame", 0, "mode"),
    CONST("field", "send one frame for each field", 1, "mode"),
    { "parity", "specify the assumed picture field parity", OFFSET(parity), AV_OPT_TYPE_INT, {.i64=-1}, -1, 1, FLAGS, .unit = "parity" },
    CONST("tff",  "assume top field first",     0, "parity"),
    CONST("bff",  "assume bottom field first",  1, "parity"),
    CONST("auto", "auto detect parity",        -1, "parity"),
    { "deint",  "specify which frames to deinterlace", OFFSET(deint), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, .unit = "deint" },
    CONST("all",        "deinterlace all frames",                       0, "deint"),
    CONST("interlaced", "only deinterlace frames marked as interlaced", 1, "deint"),
    { NULL }
};

AVFILTER_DEFINE_CLASS(w3fdif);

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV411P,
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ411P,
    AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRAP,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_GRAY9, AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12, AV_PIX_FMT_GRAY14, AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV440P10,
    AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_YUV440P12,
    AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV444P14,
    AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10, AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRP16,
    AV_PIX_FMT_YUVA444P9, AV_PIX_FMT_YUVA444P10, AV_PIX_FMT_YUVA444P12, AV_PIX_FMT_YUVA444P16,
    AV_PIX_FMT_YUVA422P9, AV_PIX_FMT_YUVA422P10, AV_PIX_FMT_YUVA422P12, AV_PIX_FMT_YUVA422P16,
    AV_PIX_FMT_YUVA420P9, AV_PIX_FMT_YUVA420P10, AV_PIX_FMT_YUVA420P16,
    AV_PIX_FMT_GBRAP10,   AV_PIX_FMT_GBRAP12,    AV_PIX_FMT_GBRAP16,
    AV_PIX_FMT_NONE
};

static void filter_simple_low(int32_t *work_line,
                              uint8_t *in_lines_cur[2],
                              const int16_t *coef, int linesize)
{
    int i;

    for (i = 0; i < linesize; i++) {
        *work_line    = *in_lines_cur[0]++ * coef[0];
        *work_line++ += *in_lines_cur[1]++ * coef[1];
    }
}

static void filter_complex_low(int32_t *work_line,
                               uint8_t *in_lines_cur[4],
                               const int16_t *coef, int linesize)
{
    int i;

    for (i = 0; i < linesize; i++) {
        *work_line    = *in_lines_cur[0]++ * coef[0];
        *work_line   += *in_lines_cur[1]++ * coef[1];
        *work_line   += *in_lines_cur[2]++ * coef[2];
        *work_line++ += *in_lines_cur[3]++ * coef[3];
    }
}

static void filter_simple_high(int32_t *work_line,
                               uint8_t *in_lines_cur[3],
                               uint8_t *in_lines_adj[3],
                               const int16_t *coef, int linesize)
{
    int i;

    for (i = 0; i < linesize; i++) {
        *work_line   += *in_lines_cur[0]++ * coef[0];
        *work_line   += *in_lines_adj[0]++ * coef[0];
        *work_line   += *in_lines_cur[1]++ * coef[1];
        *work_line   += *in_lines_adj[1]++ * coef[1];
        *work_line   += *in_lines_cur[2]++ * coef[2];
        *work_line++ += *in_lines_adj[2]++ * coef[2];
    }
}

static void filter_complex_high(int32_t *work_line,
                                uint8_t *in_lines_cur[5],
                                uint8_t *in_lines_adj[5],
                                const int16_t *coef, int linesize)
{
    int i;

    for (i = 0; i < linesize; i++) {
        *work_line   += *in_lines_cur[0]++ * coef[0];
        *work_line   += *in_lines_adj[0]++ * coef[0];
        *work_line   += *in_lines_cur[1]++ * coef[1];
        *work_line   += *in_lines_adj[1]++ * coef[1];
        *work_line   += *in_lines_cur[2]++ * coef[2];
        *work_line   += *in_lines_adj[2]++ * coef[2];
        *work_line   += *in_lines_cur[3]++ * coef[3];
        *work_line   += *in_lines_adj[3]++ * coef[3];
        *work_line   += *in_lines_cur[4]++ * coef[4];
        *work_line++ += *in_lines_adj[4]++ * coef[4];
    }
}

static void filter_scale(uint8_t *out_pixel, const int32_t *work_pixel, int linesize, int max)
{
    int j;

    for (j = 0; j < linesize; j++, out_pixel++, work_pixel++)
        *out_pixel = av_clip(*work_pixel, 0, 255 * 256 * 128) >> 15;
}

static void filter16_simple_low(int32_t *work_line,
                                uint8_t *in_lines_cur8[2],
                                const int16_t *coef, int linesize)
{
    uint16_t *in_lines_cur[2] = { (uint16_t *)in_lines_cur8[0], (uint16_t *)in_lines_cur8[1] };
    int i;

    linesize /= 2;
    for (i = 0; i < linesize; i++) {
        *work_line    = *in_lines_cur[0]++ * coef[0];
        *work_line++ += *in_lines_cur[1]++ * coef[1];
    }
}

static void filter16_complex_low(int32_t *work_line,
                                 uint8_t *in_lines_cur8[4],
                                 const int16_t *coef, int linesize)
{
    uint16_t *in_lines_cur[4] = { (uint16_t *)in_lines_cur8[0],
                                  (uint16_t *)in_lines_cur8[1],
                                  (uint16_t *)in_lines_cur8[2],
                                  (uint16_t *)in_lines_cur8[3] };
    int i;

    linesize /= 2;
    for (i = 0; i < linesize; i++) {
        *work_line    = *in_lines_cur[0]++ * coef[0];
        *work_line   += *in_lines_cur[1]++ * coef[1];
        *work_line   += *in_lines_cur[2]++ * coef[2];
        *work_line++ += *in_lines_cur[3]++ * coef[3];
    }
}

static void filter16_simple_high(int32_t *work_line,
                                 uint8_t *in_lines_cur8[3],
                                 uint8_t *in_lines_adj8[3],
                                 const int16_t *coef, int linesize)
{
    uint16_t *in_lines_cur[3] = { (uint16_t *)in_lines_cur8[0],
                                  (uint16_t *)in_lines_cur8[1],
                                  (uint16_t *)in_lines_cur8[2] };
    uint16_t *in_lines_adj[3] = { (uint16_t *)in_lines_adj8[0],
                                  (uint16_t *)in_lines_adj8[1],
                                  (uint16_t *)in_lines_adj8[2] };
    int i;

    linesize /= 2;
    for (i = 0; i < linesize; i++) {
        *work_line   += *in_lines_cur[0]++ * coef[0];
        *work_line   += *in_lines_adj[0]++ * coef[0];
        *work_line   += *in_lines_cur[1]++ * coef[1];
        *work_line   += *in_lines_adj[1]++ * coef[1];
        *work_line   += *in_lines_cur[2]++ * coef[2];
        *work_line++ += *in_lines_adj[2]++ * coef[2];
    }
}

static void filter16_complex_high(int32_t *work_line,
                                  uint8_t *in_lines_cur8[5],
                                  uint8_t *in_lines_adj8[5],
                                  const int16_t *coef, int linesize)
{
    uint16_t *in_lines_cur[5] = { (uint16_t *)in_lines_cur8[0],
                                  (uint16_t *)in_lines_cur8[1],
                                  (uint16_t *)in_lines_cur8[2],
                                  (uint16_t *)in_lines_cur8[3],
                                  (uint16_t *)in_lines_cur8[4] };
    uint16_t *in_lines_adj[5] = { (uint16_t *)in_lines_adj8[0],
                                  (uint16_t *)in_lines_adj8[1],
                                  (uint16_t *)in_lines_adj8[2],
                                  (uint16_t *)in_lines_adj8[3],
                                  (uint16_t *)in_lines_adj8[4] };
    int i;

    linesize /= 2;
    for (i = 0; i < linesize; i++) {
        *work_line   += *in_lines_cur[0]++ * coef[0];
        *work_line   += *in_lines_adj[0]++ * coef[0];
        *work_line   += *in_lines_cur[1]++ * coef[1];
        *work_line   += *in_lines_adj[1]++ * coef[1];
        *work_line   += *in_lines_cur[2]++ * coef[2];
        *work_line   += *in_lines_adj[2]++ * coef[2];
        *work_line   += *in_lines_cur[3]++ * coef[3];
        *work_line   += *in_lines_adj[3]++ * coef[3];
        *work_line   += *in_lines_cur[4]++ * coef[4];
        *work_line++ += *in_lines_adj[4]++ * coef[4];
    }
}

static void filter16_scale(uint8_t *out_pixel8, const int32_t *work_pixel, int linesize, int max)
{
    uint16_t *out_pixel = (uint16_t *)out_pixel8;
    int j;

    linesize /= 2;
    for (j = 0; j < linesize; j++, out_pixel++, work_pixel++)
        *out_pixel = av_clip(*work_pixel, 0, max) >> 15;
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    W3FDIFContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    int ret, i, depth, nb_threads;

    if ((ret = av_image_fill_linesizes(s->linesize, inlink->format, inlink->w)) < 0)
        return ret;

    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;

    if (inlink->h < 3) {
        av_log(ctx, AV_LOG_ERROR, "Video of less than 3 lines is not supported\n");
        return AVERROR(EINVAL);
    }

    s->nb_planes = av_pix_fmt_count_planes(inlink->format);
    nb_threads = ff_filter_get_nb_threads(ctx);
    s->work_line = av_calloc(nb_threads, sizeof(*s->work_line));
    if (!s->work_line)
        return AVERROR(ENOMEM);
    s->nb_threads = nb_threads;

    for (i = 0; i < s->nb_threads; i++) {
        s->work_line[i] = av_calloc(FFALIGN(s->linesize[0], 32), sizeof(*s->work_line[0]));
        if (!s->work_line[i])
            return AVERROR(ENOMEM);
    }

    depth = desc->comp[0].depth;
    s->max = ((1 << depth) - 1) * 256 * 128;
    if (depth <= 8) {
        s->dsp.filter_simple_low   = filter_simple_low;
        s->dsp.filter_complex_low  = filter_complex_low;
        s->dsp.filter_simple_high  = filter_simple_high;
        s->dsp.filter_complex_high = filter_complex_high;
        s->dsp.filter_scale        = filter_scale;
    } else {
        s->dsp.filter_simple_low   = filter16_simple_low;
        s->dsp.filter_complex_low  = filter16_complex_low;
        s->dsp.filter_simple_high  = filter16_simple_high;
        s->dsp.filter_complex_high = filter16_complex_high;
        s->dsp.filter_scale        = filter16_scale;
    }

#if ARCH_X86
    ff_w3fdif_init_x86(&s->dsp, depth);
#endif

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *il = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);
    W3FDIFContext *s = ctx->priv;

    outlink->time_base = av_mul_q(inlink->time_base, (AVRational){1, 2});
    if (s->mode)
        ol->frame_rate = av_mul_q(il->frame_rate, (AVRational){2, 1});

    return 0;
}

/*
 * Filter coefficients from PH-2071, scaled by 256 * 128.
 * Each set of coefficients has a set for low-frequencies and high-frequencies.
 * n_coef_lf[] and n_coef_hf[] are the number of coefs for simple and more-complex.
 * It is important for later that n_coef_lf[] is even and n_coef_hf[] is odd.
 * coef_lf[][] and coef_hf[][] are the coefficients for low-frequencies
 * and high-frequencies for simple and more-complex mode.
 */
static const int8_t   n_coef_lf[2] = { 2, 4 };
static const int16_t coef_lf[2][4] = {{ 16384, 16384,     0,    0},
                                      {  -852, 17236, 17236, -852}};
static const int8_t   n_coef_hf[2] = { 3, 5 };
static const int16_t coef_hf[2][5] = {{ -2048,  4096, -2048,     0,    0},
                                      {  1016, -3801,  5570, -3801, 1016}};

typedef struct ThreadData {
    AVFrame *out, *cur, *adj;
} ThreadData;

static int deinterlace_plane_slice(AVFilterContext *ctx, void *arg,
                                   int jobnr, int nb_jobs, int plane)
{
    W3FDIFContext *s = ctx->priv;
    ThreadData *td = arg;
    AVFrame *out = td->out;
    AVFrame *cur = td->cur;
    AVFrame *adj = td->adj;
    const int filter = s->filter;
    uint8_t *in_line, *in_lines_cur[5], *in_lines_adj[5];
    uint8_t *out_line, *out_pixel;
    int32_t *work_line, *work_pixel;
    uint8_t *cur_data = cur->data[plane];
    uint8_t *adj_data = adj->data[plane];
    uint8_t *dst_data = out->data[plane];
    const int linesize = s->linesize[plane];
    const int height   = s->planeheight[plane];
    const int cur_line_stride = cur->linesize[plane];
    const int adj_line_stride = adj->linesize[plane];
    const int dst_line_stride = out->linesize[plane];
    const int start = (height * jobnr) / nb_jobs;
    const int end = (height * (jobnr+1)) / nb_jobs;
    const int max = s->max;
    const int interlaced = !!(cur->flags & AV_FRAME_FLAG_INTERLACED);
    const int tff = s->field == (s->parity == -1 ? interlaced ? !!(cur->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) : 1 :
                                 s->parity ^ 1);
    int j, y_in, y_out;

    /* copy unchanged the lines of the field */
    y_out = start + (tff ^ (start & 1));

    in_line  = cur_data + (y_out * cur_line_stride);
    out_line = dst_data + (y_out * dst_line_stride);

    while (y_out < end) {
        memcpy(out_line, in_line, linesize);
        y_out += 2;
        in_line  += cur_line_stride * 2;
        out_line += dst_line_stride * 2;
    }

    /* interpolate other lines of the field */
    y_out = start + ((!tff) ^ (start & 1));

    out_line = dst_data + (y_out * dst_line_stride);

    while (y_out < end) {
        /* get low vertical frequencies from current field */
        for (j = 0; j < n_coef_lf[filter]; j++) {
            y_in = (y_out + 1) + (j * 2) - n_coef_lf[filter];

            while (y_in < 0)
                y_in += 2;
            while (y_in >= height)
                y_in -= 2;

            in_lines_cur[j] = cur_data + (y_in * cur_line_stride);
        }

        work_line = s->work_line[jobnr];
        switch (n_coef_lf[filter]) {
        case 2:
            s->dsp.filter_simple_low(work_line, in_lines_cur,
                                     coef_lf[filter], linesize);
            break;
        case 4:
            s->dsp.filter_complex_low(work_line, in_lines_cur,
                                      coef_lf[filter], linesize);
        }

        /* get high vertical frequencies from adjacent fields */
        for (j = 0; j < n_coef_hf[filter]; j++) {
            y_in = (y_out + 1) + (j * 2) - n_coef_hf[filter];

            while (y_in < 0)
                y_in += 2;
            while (y_in >= height)
                y_in -= 2;

            in_lines_cur[j] = cur_data + (y_in * cur_line_stride);
            in_lines_adj[j] = adj_data + (y_in * adj_line_stride);
        }

        work_line = s->work_line[jobnr];
        switch (n_coef_hf[filter]) {
        case 3:
            s->dsp.filter_simple_high(work_line, in_lines_cur, in_lines_adj,
                                      coef_hf[filter], linesize);
            break;
        case 5:
            s->dsp.filter_complex_high(work_line, in_lines_cur, in_lines_adj,
                                       coef_hf[filter], linesize);
        }

        /* save scaled result to the output frame, scaling down by 256 * 128 */
        work_pixel = s->work_line[jobnr];
        out_pixel = out_line;

        s->dsp.filter_scale(out_pixel, work_pixel, linesize, max);

        /* move on to next line */
        y_out += 2;
        out_line += dst_line_stride * 2;
    }

    return 0;
}

static int deinterlace_slice(AVFilterContext *ctx, void *arg,
                             int jobnr, int nb_jobs)
{
    W3FDIFContext *s = ctx->priv;

    for (int p = 0; p < s->nb_planes; p++)
        deinterlace_plane_slice(ctx, arg, jobnr, nb_jobs, p);

    return 0;
}

static int filter(AVFilterContext *ctx, int is_second)
{
    W3FDIFContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out, *adj;
    ThreadData td;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return AVERROR(ENOMEM);
    av_frame_copy_props(out, s->cur);
    out->flags &= ~AV_FRAME_FLAG_INTERLACED;

    if (!is_second) {
        if (out->pts != AV_NOPTS_VALUE)
            out->pts *= 2;
    } else {
        int64_t cur_pts  = s->cur->pts;
        int64_t next_pts = s->next->pts;

        if (next_pts != AV_NOPTS_VALUE && cur_pts != AV_NOPTS_VALUE) {
            out->pts = cur_pts + next_pts;
        } else {
            out->pts = AV_NOPTS_VALUE;
        }
    }

    adj = s->field ? s->next : s->prev;
    td.out = out; td.cur = s->cur; td.adj = adj;
    ff_filter_execute(ctx, deinterlace_slice, &td, NULL,
                      FFMIN(s->planeheight[1], s->nb_threads));

    if (s->mode)
        s->field = !s->field;

    return ff_filter_frame(outlink, out);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    W3FDIFContext *s = ctx->priv;
    int ret;

    av_frame_free(&s->prev);
    s->prev = s->cur;
    s->cur  = s->next;
    s->next = frame;

    if (!s->cur) {
        s->cur = av_frame_clone(s->next);
        if (!s->cur)
            return AVERROR(ENOMEM);
    }

    if (!s->prev)
        return 0;

    if ((s->deint && !(s->cur->flags & AV_FRAME_FLAG_INTERLACED)) || ff_filter_disabled(ctx)) {
        AVFrame *out = av_frame_clone(s->cur);
        if (!out)
            return AVERROR(ENOMEM);

        av_frame_free(&s->prev);
        if (out->pts != AV_NOPTS_VALUE)
            out->pts *= 2;
        return ff_filter_frame(ctx->outputs[0], out);
    }

    ret = filter(ctx, 0);
    if (ret < 0 || s->mode == 0)
        return ret;

    return filter(ctx, 1);
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    W3FDIFContext *s = ctx->priv;
    int ret;

    if (s->eof)
        return AVERROR_EOF;

    ret = ff_request_frame(ctx->inputs[0]);

    if (ret == AVERROR_EOF && s->cur) {
        AVFrame *next = av_frame_clone(s->next);
        if (!next)
            return AVERROR(ENOMEM);
        next->pts = s->next->pts * 2 - s->cur->pts;
        filter_frame(ctx->inputs[0], next);
        s->eof = 1;
    } else if (ret < 0) {
        return ret;
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    W3FDIFContext *s = ctx->priv;
    int i;

    av_frame_free(&s->prev);
    av_frame_free(&s->cur );
    av_frame_free(&s->next);

    for (i = 0; i < s->nb_threads; i++)
        av_freep(&s->work_line[i]);

    av_freep(&s->work_line);
}

static const AVFilterPad w3fdif_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .filter_frame  = filter_frame,
        .config_props  = config_input,
    },
};

static const AVFilterPad w3fdif_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
};

const FFFilter ff_vf_w3fdif = {
    .p.name        = "w3fdif",
    .p.description = NULL_IF_CONFIG_SMALL("Apply Martin Weston three field deinterlace."),
    .p.priv_class  = &w3fdif_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL | AVFILTER_FLAG_SLICE_THREADS,
    .priv_size     = sizeof(W3FDIFContext),
    .uninit        = uninit,
    FILTER_INPUTS(w3fdif_inputs),
    FILTER_OUTPUTS(w3fdif_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .process_command = ff_filter_process_command,
};
