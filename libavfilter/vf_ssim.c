/*
 * Copyright (c) 2003-2013 Loren Merritt
 * Copyright (c) 2015 Paul B Mahol
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

/* Computes the Structural Similarity Metric between two video streams.
 * original algorithm:
 * Z. Wang, A. C. Bovik, H. R. Sheikh and E. P. Simoncelli,
 *   "Image quality assessment: From error visibility to structural similarity,"
 *   IEEE Transactions on Image Processing, vol. 13, no. 4, pp. 600-612, Apr. 2004.
 *
 * To improve speed, this implementation uses the standard approximation of
 * overlapped 8x8 block sums, rather than the original gaussian weights.
 */

/*
 * @file
 * Calculate the SSIM between two input videos.
 */

#include "config.h"

#include "libavutil/avstring.h"
#include "libavutil/file_open.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/refstruct.h"
#include "libavutil/thread.h"
#include "libavutil/threadprogress.h"

#include "avfilter.h"
#include "drawutils.h"
#include "filters.h"
#include "framesync.h"
#include "ssim.h"

// data shared between frame threads
typedef struct SSIMShared {
    double ssim[4], ssim_total;
} SSIMShared;

typedef struct SSIMContext {
    const AVClass *class;
    FFFrameSync fs;
    FILE *stats_file;
    char *stats_file_str;
    int nb_components;
    int nb_slice_threads;
    int max;
    uint64_t nb_frames;
    char comps[4];
    double coefs[4];
    uint8_t rgba_map[4];
    int planewidth[4];
    int planeheight[4];
    int **temp;
    int is_rgb;
    double **score;
    int (*ssim_plane)(AVFilterContext *ctx, void *arg,
                      int jobnr, int nb_jobs);
    SSIMDSPContext dsp;

    // RefStruct references
    SSIMShared      *shared;
    AVRefStructPool *progress_pool;
    ThreadProgress  *prev_progress;
    ThreadProgress  *progress;

    AVFrame        *frame_master;
    AVFrame        *frame_ref;
} SSIMContext;

#define OFFSET(x) offsetof(SSIMContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption ssim_options[] = {
    {"stats_file", "Set file where to store per-frame difference information", OFFSET(stats_file_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    {"f",          "Set file where to store per-frame difference information", OFFSET(stats_file_str), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(ssim, SSIMContext, fs);

static void set_meta(AVDictionary **metadata, const char *key, char comp, float d)
{
    char value[128];
    snprintf(value, sizeof(value), "%f", d);
    if (comp) {
        char key2[128];
        snprintf(key2, sizeof(key2), "%s%c", key, comp);
        av_dict_set(metadata, key2, value, 0);
    } else {
        av_dict_set(metadata, key, value, 0);
    }
}

static void ssim_4x4xn_16bit(const uint8_t *main8, ptrdiff_t main_stride,
                             const uint8_t *ref8, ptrdiff_t ref_stride,
                             int64_t (*sums)[4], int width)
{
    const uint16_t *main16 = (const uint16_t *)main8;
    const uint16_t *ref16  = (const uint16_t *)ref8;
    int x, y, z;

    main_stride >>= 1;
    ref_stride >>= 1;

    for (z = 0; z < width; z++) {
        uint64_t s1 = 0, s2 = 0, ss = 0, s12 = 0;

        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                unsigned a = main16[x + y * main_stride];
                unsigned b = ref16[x + y * ref_stride];

                s1  += a;
                s2  += b;
                ss  += a*a;
                ss  += b*b;
                s12 += a*b;
            }
        }

        sums[z][0] = s1;
        sums[z][1] = s2;
        sums[z][2] = ss;
        sums[z][3] = s12;
        main16 += 4;
        ref16 += 4;
    }
}

static void ssim_4x4xn_8bit(const uint8_t *main, ptrdiff_t main_stride,
                            const uint8_t *ref, ptrdiff_t ref_stride,
                            int (*sums)[4], int width)
{
    int x, y, z;

    for (z = 0; z < width; z++) {
        uint32_t s1 = 0, s2 = 0, ss = 0, s12 = 0;

        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                int a = main[x + y * main_stride];
                int b = ref[x + y * ref_stride];

                s1  += a;
                s2  += b;
                ss  += a*a;
                ss  += b*b;
                s12 += a*b;
            }
        }

        sums[z][0] = s1;
        sums[z][1] = s2;
        sums[z][2] = ss;
        sums[z][3] = s12;
        main += 4;
        ref += 4;
    }
}

static float ssim_end1x(int64_t s1, int64_t s2, int64_t ss, int64_t s12, int max)
{
    int64_t ssim_c1 = (int64_t)(.01*.01*max*max*64 + .5);
    int64_t ssim_c2 = (int64_t)(.03*.03*max*max*64*63 + .5);

    int64_t fs1 = s1;
    int64_t fs2 = s2;
    int64_t fss = ss;
    int64_t fs12 = s12;
    int64_t vars = fss * 64 - fs1 * fs1 - fs2 * fs2;
    int64_t covar = fs12 * 64 - fs1 * fs2;

    return (float)(2 * fs1 * fs2 + ssim_c1) * (float)(2 * covar + ssim_c2)
         / ((float)(fs1 * fs1 + fs2 * fs2 + ssim_c1) * (float)(vars + ssim_c2));
}

static float ssim_end1(int s1, int s2, int ss, int s12)
{
    static const int ssim_c1 = (int)(.01*.01*255*255*64 + .5);
    static const int ssim_c2 = (int)(.03*.03*255*255*64*63 + .5);

    int fs1 = s1;
    int fs2 = s2;
    int fss = ss;
    int fs12 = s12;
    int vars = fss * 64 - fs1 * fs1 - fs2 * fs2;
    int covar = fs12 * 64 - fs1 * fs2;

    return (float)(2 * fs1 * fs2 + ssim_c1) * (float)(2 * covar + ssim_c2)
         / ((float)(fs1 * fs1 + fs2 * fs2 + ssim_c1) * (float)(vars + ssim_c2));
}

static float ssim_endn_16bit(const int64_t (*sum0)[4], const int64_t (*sum1)[4], int width, int max)
{
    float ssim = 0.0;

    for (int i = 0; i < width; i++)
        ssim += ssim_end1x(sum0[i][0] + sum0[i + 1][0] + sum1[i][0] + sum1[i + 1][0],
                           sum0[i][1] + sum0[i + 1][1] + sum1[i][1] + sum1[i + 1][1],
                           sum0[i][2] + sum0[i + 1][2] + sum1[i][2] + sum1[i + 1][2],
                           sum0[i][3] + sum0[i + 1][3] + sum1[i][3] + sum1[i + 1][3],
                           max);
    return ssim;
}

static double ssim_endn_8bit(const int (*sum0)[4], const int (*sum1)[4], int width)
{
    double ssim = 0.0;

    for (int i = 0; i < width; i++)
        ssim += ssim_end1(sum0[i][0] + sum0[i + 1][0] + sum1[i][0] + sum1[i + 1][0],
                          sum0[i][1] + sum0[i + 1][1] + sum1[i][1] + sum1[i + 1][1],
                          sum0[i][2] + sum0[i + 1][2] + sum1[i][2] + sum1[i + 1][2],
                          sum0[i][3] + sum0[i + 1][3] + sum1[i][3] + sum1[i + 1][3]);
    return ssim;
}

#define SUM_LEN(w) (((w) >> 2) + 3)

typedef struct ThreadData {
    const uint8_t *main_data[4];
    const uint8_t *ref_data[4];
    int main_linesize[4];
    int ref_linesize[4];
    int planewidth[4];
    int planeheight[4];
    double **score;
    int **temp;
    int nb_components;
    int max;
    SSIMDSPContext *dsp;
} ThreadData;

static int ssim_plane_16bit(AVFilterContext *ctx, void *arg,
                            int jobnr, int nb_jobs)
{
    ThreadData *td = arg;
    double *score = td->score[jobnr];
    void *temp = td->temp[jobnr];
    const int max = td->max;

    for (int c = 0; c < td->nb_components; c++) {
        const uint8_t *main_data = td->main_data[c];
        const uint8_t *ref_data = td->ref_data[c];
        const int main_stride = td->main_linesize[c];
        const int ref_stride = td->ref_linesize[c];
        int width = td->planewidth[c];
        int height = td->planeheight[c];
        const int slice_start = ((height >> 2) * jobnr) / nb_jobs;
        const int slice_end = ((height >> 2) * (jobnr+1)) / nb_jobs;
        const int ystart = FFMAX(1, slice_start);
        int z = ystart - 1;
        double ssim = 0.0;
        int64_t (*sum0)[4] = temp;
        int64_t (*sum1)[4] = sum0 + SUM_LEN(width);

        width >>= 2;
        height >>= 2;

        for (int y = ystart; y < slice_end; y++) {
            for (; z <= y; z++) {
                FFSWAP(void*, sum0, sum1);
                ssim_4x4xn_16bit(&main_data[4 * z * main_stride], main_stride,
                                 &ref_data[4 * z * ref_stride], ref_stride,
                                 sum0, width);
            }

            ssim += ssim_endn_16bit((const int64_t (*)[4])sum0, (const int64_t (*)[4])sum1, width - 1, max);
        }

        score[c] = ssim;
    }

    return 0;
}

static int ssim_plane(AVFilterContext *ctx, void *arg,
                      int jobnr, int nb_jobs)
{
    ThreadData *td = arg;
    double *score = td->score[jobnr];
    void *temp = td->temp[jobnr];
    SSIMDSPContext *dsp = td->dsp;

    for (int c = 0; c < td->nb_components; c++) {
        const uint8_t *main_data = td->main_data[c];
        const uint8_t *ref_data = td->ref_data[c];
        const int main_stride = td->main_linesize[c];
        const int ref_stride = td->ref_linesize[c];
        int width = td->planewidth[c];
        int height = td->planeheight[c];
        const int slice_start = ((height >> 2) * jobnr) / nb_jobs;
        const int slice_end = ((height >> 2) * (jobnr+1)) / nb_jobs;
        const int ystart = FFMAX(1, slice_start);
        int z = ystart - 1;
        double ssim = 0.0;
        int (*sum0)[4] = temp;
        int (*sum1)[4] = sum0 + SUM_LEN(width);

        width >>= 2;
        height >>= 2;

        for (int y = ystart; y < slice_end; y++) {
            for (; z <= y; z++) {
                FFSWAP(void*, sum0, sum1);
                dsp->ssim_4x4_line(&main_data[4 * z * main_stride], main_stride,
                                   &ref_data[4 * z * ref_stride], ref_stride,
                                   sum0, width);
            }

            ssim += dsp->ssim_end_line((const int (*)[4])sum0, (const int (*)[4])sum1, width - 1);
        }

        score[c] = ssim;
    }

    return 0;
}

static double ssim_db(double ssim, double weight)
{
    return (fabs(weight - ssim) > 1e-9) ? 10.0 * log10(weight / (weight - ssim)) : INFINITY;
}

static int do_ssim(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    SSIMContext *s = ctx->priv;
    AVFrame *master = s->frame_master;
    AVFrame    *ref = s->frame_ref;
    AVDictionary **metadata;
    double c[4] = {0}, ssimv = 0.0;
    ThreadData td;
    int ret = 0, i;

    if (ff_filter_disabled(ctx) || !ref)
        goto finish;
    metadata = &master->metadata;

    td.nb_components = s->nb_components;
    td.dsp = &s->dsp;
    td.score = s->score;
    td.temp = s->temp;
    td.max = s->max;

    for (int n = 0; n < s->nb_components; n++) {
        td.main_data[n] = master->data[n];
        td.ref_data[n] = ref->data[n];
        td.main_linesize[n] = master->linesize[n];
        td.ref_linesize[n] = ref->linesize[n];
        td.planewidth[n] = s->planewidth[n];
        td.planeheight[n] = s->planeheight[n];
    }

    if (master->color_range != ref->color_range) {
        av_log(ctx, AV_LOG_WARNING, "master and reference "
               "frames use different color ranges (%s != %s)\n",
               av_color_range_name(master->color_range),
               av_color_range_name(ref->color_range));
    }

    ff_filter_execute(ctx, s->ssim_plane, &td, NULL,
                      FFMIN((s->planeheight[1] + 3) >> 2, s->nb_slice_threads));

#if CONFIG_AVFILTER_THREAD_FRAME
    if (s->prev_progress)
        ff_thread_progress_await(s->prev_progress, INT_MAX);
#endif

    for (i = 0; i < s->nb_components; i++) {
        for (int j = 0; j < s->nb_slice_threads; j++)
            c[i] += s->score[j][i];
        c[i] = c[i] / (((s->planewidth[i] >> 2) - 1) * ((s->planeheight[i] >> 2) - 1));
    }

    for (i = 0; i < s->nb_components; i++) {
        ssimv += s->coefs[i] * c[i];
        s->shared->ssim[i] += c[i];
    }

    for (i = 0; i < s->nb_components; i++) {
        int cidx = s->is_rgb ? s->rgba_map[i] : i;
        set_meta(metadata, "lavfi.ssim.", s->comps[i], c[cidx]);
    }
    s->shared->ssim_total += ssimv;

    set_meta(metadata, "lavfi.ssim.All", 0, ssimv);
    set_meta(metadata, "lavfi.ssim.dB", 0, ssim_db(ssimv, 1.0));

    if (s->stats_file) {
        fprintf(s->stats_file, "n:%"PRId64" ", s->nb_frames);

        for (i = 0; i < s->nb_components; i++) {
            int cidx = s->is_rgb ? s->rgba_map[i] : i;
            fprintf(s->stats_file, "%c:%f ", s->comps[i], c[cidx]);
        }

        fprintf(s->stats_file, "All:%f (%f)\n", ssimv, ssim_db(ssimv, 1.0));
    }

finish:
#if CONFIG_AVFILTER_THREAD_FRAME
    if (s->progress)
        ff_thread_progress_report(s->progress, INT_MAX);
#endif

    if (ret < 0)
        return ret;

    s->frame_master = NULL;

    return ff_filter_frame(ctx->outputs[0], master);
}

static av_cold int init(AVFilterContext *ctx)
{
    SSIMContext *s = ctx->priv;

    if (!ff_filter_is_frame_thread(ctx)) {
        s->shared = av_refstruct_allocz(sizeof(SSIMShared));
        if (!s->shared)
            return AVERROR(ENOMEM);
    }

    if (s->stats_file_str && !ff_filter_is_frame_thread(ctx)) {
        if (!strcmp(s->stats_file_str, "-")) {
            s->stats_file = stdout;
        } else {
            s->stats_file = avpriv_fopen_utf8(s->stats_file_str, "w");
            if (!s->stats_file) {
                int err = AVERROR(errno);
                av_log(ctx, AV_LOG_ERROR, "Could not open stats file %s: %s\n",
                       s->stats_file_str, av_err2str(err));
                return err;
            }
        }
    }

    s->fs.on_event = do_ssim;
    return 0;
}

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY9, AV_PIX_FMT_GRAY10,
    AV_PIX_FMT_GRAY12, AV_PIX_FMT_GRAY14, AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_GBRP,
#define PF(suf) AV_PIX_FMT_YUV420##suf,  AV_PIX_FMT_YUV422##suf,  AV_PIX_FMT_YUV444##suf, AV_PIX_FMT_GBR##suf
    PF(P9), PF(P10), PF(P12), PF(P14), PF(P16),
    AV_PIX_FMT_NONE
};

static int config_input_ref(AVFilterLink *inlink)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    AVFilterContext *ctx  = inlink->dst;
    SSIMContext *s = ctx->priv;
    int sum = 0;

    s->nb_slice_threads = (ctx->thread_type & AVFILTER_THREAD_SLICE) ?
                          ff_filter_get_nb_threads(ctx) : 1;
    s->nb_components = desc->nb_components;

    if (ctx->inputs[0]->w != ctx->inputs[1]->w ||
        ctx->inputs[0]->h != ctx->inputs[1]->h) {
        av_log(ctx, AV_LOG_ERROR, "Width and height of input videos must be same.\n");
        return AVERROR(EINVAL);
    }

    s->is_rgb = ff_fill_rgba_map(s->rgba_map, inlink->format) >= 0;
    s->comps[0] = s->is_rgb ? 'R' : 'Y';
    s->comps[1] = s->is_rgb ? 'G' : 'U';
    s->comps[2] = s->is_rgb ? 'B' : 'V';
    s->comps[3] = 'A';

    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;
    s->planewidth[1]  = s->planewidth[2]  = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0]  = s->planewidth[3]  = inlink->w;
    for (int i = 0; i < s->nb_components; i++)
        sum += s->planeheight[i] * s->planewidth[i];
    for (int i = 0; i < s->nb_components; i++)
        s->coefs[i] = (double) s->planeheight[i] * s->planewidth[i] / sum;

    s->temp = av_calloc(s->nb_slice_threads, sizeof(*s->temp));
    if (!s->temp)
        return AVERROR(ENOMEM);

    for (int t = 0; t < s->nb_slice_threads; t++) {
        s->temp[t] = av_calloc(2 * SUM_LEN(inlink->w), (desc->comp[0].depth > 8) ? sizeof(int64_t[4]) : sizeof(int[4]));
        if (!s->temp[t])
            return AVERROR(ENOMEM);
    }
    s->max = (1 << desc->comp[0].depth) - 1;

    s->ssim_plane = desc->comp[0].depth > 8 ? ssim_plane_16bit : ssim_plane;
    s->dsp.ssim_4x4_line = ssim_4x4xn_8bit;
    s->dsp.ssim_end_line = ssim_endn_8bit;
#if ARCH_X86
    ff_ssim_init_x86(&s->dsp);
#endif

    s->score = av_calloc(s->nb_slice_threads, sizeof(*s->score));
    if (!s->score)
        return AVERROR(ENOMEM);

    for (int t = 0; t < s->nb_slice_threads; t++) {
        s->score[t] = av_calloc(s->nb_components, sizeof(*s->score[0]));
        if (!s->score[t])
            return AVERROR(ENOMEM);
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    SSIMContext *s = ctx->priv;
    AVFilterLink *mainlink = ctx->inputs[0];
    FilterLink *il = ff_filter_link(mainlink);
    FilterLink *ol = ff_filter_link(outlink);
    int ret;

    ret = ff_framesync_init_dualinput(&s->fs, ctx);
    if (ret < 0)
        return ret;
    outlink->w = mainlink->w;
    outlink->h = mainlink->h;
    outlink->time_base = mainlink->time_base;
    outlink->sample_aspect_ratio = mainlink->sample_aspect_ratio;
    ol->frame_rate = il->frame_rate;

    if ((ret = ff_framesync_configure(&s->fs)) < 0)
        return ret;

    outlink->time_base = s->fs.time_base;

    if (av_cmp_q(mainlink->time_base, outlink->time_base) ||
        av_cmp_q(ctx->inputs[1]->time_base, outlink->time_base))
        av_log(ctx, AV_LOG_WARNING, "not matching timebases found between first input: %d/%d and second input %d/%d, results may be incorrect!\n",
               mainlink->time_base.num, mainlink->time_base.den,
               ctx->inputs[1]->time_base.num, ctx->inputs[1]->time_base.den);

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    SSIMContext *s = ctx->priv;
    return ff_framesync_activate_frames(&s->fs);
}

#if CONFIG_AVFILTER_THREAD_FRAME
static int ssim_transfer_state(AVFilterContext *dst, const AVFilterContext *src)
{
    SSIMContext       *s_dst = dst->priv;
    const SSIMContext *s_src = src->priv;

    // only transfer state from main thread to workers
    if (!ff_filter_is_frame_thread(dst) || ff_filter_is_frame_thread(src))
        return 0;

    s_dst->stats_file = s_src->stats_file;

    s_dst->nb_frames = s_src->nb_frames;

    av_refstruct_replace(&s_dst->shared,        s_src->shared);
    av_refstruct_replace(&s_dst->prev_progress, s_src->prev_progress);
    av_refstruct_replace(&s_dst->progress,      s_src->progress);

    av_frame_free(&s_dst->frame_master);
    av_frame_free(&s_dst->frame_ref);

    s_dst->frame_master = av_frame_clone(s_src->frame_master);
    s_dst->frame_ref    = av_frame_clone(s_src->frame_ref);
    if (!s_dst->frame_master || !s_dst->frame_ref)
        return AVERROR(ENOMEM);

    return 0;
}

static int progress_init(AVRefStructOpaque opaque, void *obj)
{
    return ff_thread_progress_init(obj, 1);
}

static void progress_reset(AVRefStructOpaque opaque, void *obj)
{
    ff_thread_progress_reset(obj);
}

static void progress_free(AVRefStructOpaque opaque, void *obj)
{
    ff_thread_progress_destroy(obj);
}
#endif

static int ssim_filter_prepare(AVFilterContext *ctx)
{
    SSIMContext *s = ctx->priv;
    AVFrame *ref;
    int ret;

    ret = ff_framesync_filter_prepare(&s->fs);
    if (ret < 0)
        return ret;

    av_frame_free(&s->frame_master);
    av_frame_free(&s->frame_ref);

    ret = ff_framesync_dualinput_get(&s->fs, &s->frame_master, &ref);
    if (ret < 0)
        return ret;

    if (ref) {
        s->frame_ref = av_frame_clone(ref);
        if (!s->frame_ref)
            return AVERROR(ENOMEM);
    }

#if CONFIG_AVFILTER_THREAD_FRAME
    if (ctx->thread_type & AVFILTER_THREAD_FRAME_FILTER) {
        if (!s->progress_pool) {
            s->progress_pool = av_refstruct_pool_alloc_ext(sizeof(ThreadProgress), 0, NULL,
                                                           progress_init, progress_reset,
                                                           progress_free, NULL);
            if (!s->progress_pool)
                return AVERROR(ENOMEM);
        }

        av_refstruct_unref(&s->prev_progress);
        s->prev_progress = s->progress;

        s->progress = av_refstruct_pool_get(s->progress_pool);
        if (!s->progress)
            return AVERROR(ENOMEM);
    }
#endif

    s->nb_frames++;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SSIMContext *s = ctx->priv;
    SSIMShared *ss = s->shared;

    if (s->nb_frames > 0 && !ff_filter_is_frame_thread(ctx)) {
        char buf[256];
        buf[0] = 0;
        for (int i = 0; i < s->nb_components; i++) {
            int c = s->is_rgb ? s->rgba_map[i] : i;
            av_strlcatf(buf, sizeof(buf), " %c:%f (%f)", s->comps[i],
                        ss->ssim[c] / s->nb_frames,
                        ssim_db(ss->ssim[c], s->nb_frames));
        }
        av_log(ctx, AV_LOG_INFO, "SSIM%s All:%f (%f)\n", buf,
               ss->ssim_total / s->nb_frames,
               ssim_db(ss->ssim_total, s->nb_frames));
    }

    ff_framesync_uninit(&s->fs);

    if (s->stats_file && s->stats_file != stdout && !ff_filter_is_frame_thread(ctx))
        fclose(s->stats_file);

    for (int t = 0; t < s->nb_slice_threads && s->score; t++)
        av_freep(&s->score[t]);
    av_freep(&s->score);

    for (int t = 0; t < s->nb_slice_threads && s->temp; t++)
        av_freep(&s->temp[t]);
    av_freep(&s->temp);

    av_refstruct_unref(&s->shared);
    av_refstruct_unref(&s->progress_pool);
    av_refstruct_unref(&s->prev_progress);
    av_refstruct_unref(&s->progress);

    av_frame_free(&s->frame_master);
    av_frame_free(&s->frame_ref);
}

static const AVFilterPad ssim_inputs[] = {
    {
        .name         = "main",
        .type         = AVMEDIA_TYPE_VIDEO,
    },{
        .name         = "reference",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_input_ref,
    },
};

static const AVFilterPad ssim_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const FFFilter ff_vf_ssim = {
    .p.name        = "ssim",
    .p.description = NULL_IF_CONFIG_SMALL("Calculate the SSIM between two video streams."),
    .p.priv_class  = &ssim_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL |
                     AVFILTER_FLAG_SLICE_THREADS             |
                     AVFILTER_FLAG_FRAME_THREADS             |
                     AVFILTER_FLAG_METADATA_ONLY,
    .preinit       = ssim_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    .filter_prepare = ssim_filter_prepare,
#if CONFIG_AVFILTER_THREAD_FRAME
    .transfer_state       = ssim_transfer_state,
#endif
    .priv_size     = sizeof(SSIMContext),
    FILTER_INPUTS(ssim_inputs),
    FILTER_OUTPUTS(ssim_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
};
