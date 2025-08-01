/*
 * Copyright (c) 2020 Paul B Mahol
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

#include "libavutil/cpu.h"
#include "libavutil/channel_layout.h"
#include "libavutil/ffmath.h"
#include "libavutil/eval.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/tx.h"
#include "audio.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "window_func.h"

enum {
    INTERP_LINEAR,
    INTERP_CUBIC,
    INTERP_SPLINE,
    NB_INTERP
};

typedef struct AudioFIRSourceContext {
    const AVClass *class;

    float *freq_points_opt;
    unsigned nb_freq;
    float *magnitude_opt;
    unsigned nb_magnitude;
    float *phase_opt;
    unsigned nb_phase;
    int nb_taps;
    int sample_rate;
    int nb_samples;
    int win_func;
    int preset;
    int interp;
    int phaset;

    AVComplexFloat *complexf;
    float *freq;
    float *magnitude;
    int freq_size;
    int magnitude_size;
    int phase_size;

    float *taps;
    float *win;
    int64_t pts;

    AVTXContext *tx_ctx, *itx_ctx;
    av_tx_fn tx_fn, itx_fn;
} AudioFIRSourceContext;

#define OFFSET(x) offsetof(AudioFIRSourceContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define AR AV_OPT_TYPE_FLAG_ARRAY

static const AVOptionArrayDef def_freqs = {.def="0 1",.size_min=2,.sep=' '};
static const AVOptionArrayDef def_mags  = {.def="1 1",.size_min=2,.sep=' '};
static const AVOptionArrayDef def_phase = {.def="0 0",.size_min=2,.sep=' '};

static const AVOption afirsrc_options[] = {
    { "taps",      "set number of taps",   OFFSET(nb_taps),         AV_OPT_TYPE_INT,    {.i64=1025}, 9, UINT16_MAX, FLAGS },
    { "t",         "set number of taps",   OFFSET(nb_taps),         AV_OPT_TYPE_INT,    {.i64=1025}, 9, UINT16_MAX, FLAGS },
    { "frequency", "set frequency points", OFFSET(freq_points_opt), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_freqs}, 0, 1, FLAGS },
    { "f",         "set frequency points", OFFSET(freq_points_opt), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_freqs}, 0, 1, FLAGS },
    { "magnitude", "set magnitude values", OFFSET(magnitude_opt),   AV_OPT_TYPE_FLOAT|AR, {.arr=&def_mags},  0, 1, FLAGS },
    { "m",         "set magnitude values", OFFSET(magnitude_opt),   AV_OPT_TYPE_FLOAT|AR, {.arr=&def_mags},  0, 1, FLAGS },
    { "phase",     "set phase values",     OFFSET(phase_opt),       AV_OPT_TYPE_FLOAT|AR, {.arr=&def_phase},-1, 1, FLAGS },
    { "p",         "set phase values",     OFFSET(phase_opt),       AV_OPT_TYPE_FLOAT|AR, {.arr=&def_phase},-1, 1, FLAGS },
    { "sample_rate", "set sample rate",    OFFSET(sample_rate), AV_OPT_TYPE_INT, {.i64=44100},  1, INT_MAX,    FLAGS },
    { "r",           "set sample rate",    OFFSET(sample_rate), AV_OPT_TYPE_INT, {.i64=44100},  1, INT_MAX,    FLAGS },
    { "nb_samples", "set the number of samples per requested frame", OFFSET(nb_samples), AV_OPT_TYPE_INT, {.i64 = 1024}, 1, INT_MAX, FLAGS },
    { "n",          "set the number of samples per requested frame", OFFSET(nb_samples), AV_OPT_TYPE_INT, {.i64 = 1024}, 1, INT_MAX, FLAGS },
    WIN_FUNC_OPTION("win_func", OFFSET(win_func), FLAGS, WFUNC_BLACKMAN),
    WIN_FUNC_OPTION("w",        OFFSET(win_func), FLAGS, WFUNC_BLACKMAN),
    {NULL}
};

AVFILTER_DEFINE_CLASS(afirsrc);

static av_cold int init(AVFilterContext *ctx)
{
    AudioFIRSourceContext *s = ctx->priv;

    if (!(s->nb_taps & 1)) {
        av_log(s, AV_LOG_WARNING, "Number of taps %d must be odd length.\n", s->nb_taps);
        s->nb_taps |= 1;
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AudioFIRSourceContext *s = ctx->priv;

    av_freep(&s->win);
    av_freep(&s->taps);
    av_freep(&s->freq);
    av_freep(&s->magnitude);
    av_freep(&s->complexf);
    av_tx_uninit(&s->tx_ctx);
    av_tx_uninit(&s->itx_ctx);
}

static av_cold int query_formats(const AVFilterContext *ctx,
                                 AVFilterFormatsConfig **cfg_in,
                                 AVFilterFormatsConfig **cfg_out)
{
    const AudioFIRSourceContext *s = ctx->priv;
    static const AVChannelLayout chlayouts[] = { AV_CHANNEL_LAYOUT_MONO, { 0 } };
    int sample_rates[] = { s->sample_rate, -1 };
    static const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_NONE
    };
    int ret = ff_set_common_formats_from_list2(ctx, cfg_in, cfg_out, sample_fmts);
    if (ret < 0)
        return ret;

    ret = ff_set_common_channel_layouts_from_list2(ctx, cfg_in, cfg_out, chlayouts);
    if (ret < 0)
        return ret;

    return ff_set_common_samplerates_from_list2(ctx, cfg_in, cfg_out, sample_rates);
}

static void lininterp(AVComplexFloat *complexf,
                      const float *freq,
                      const float *magnitude,
                      const float *phase,
                      int m, int minterp)
{
    for (int i = 0; i < minterp; i++) {
        for (int j = 1; j < m; j++) {
            const float x = i / (float)minterp;

            if (x <= freq[j]) {
                const float mg = (x - freq[j-1]) / (freq[j] - freq[j-1]) * (magnitude[j] - magnitude[j-1]) + magnitude[j-1];
                const float ph = (x - freq[j-1]) / (freq[j] - freq[j-1]) * (phase[j] - phase[j-1]) + phase[j-1];

                complexf[i].re = mg * cosf(ph * M_PI);
                complexf[i].im = mg * sinf(ph * M_PI);
                break;
            }
        }
    }
}

static av_cold int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioFIRSourceContext *s = ctx->priv;
    float overlap, scale, *output;
    int fft_size, middle, ret;

    if (s->nb_freq != s->nb_magnitude && s->nb_freq != s->nb_phase && s->nb_freq >= 2) {
        av_log(ctx, AV_LOG_ERROR, "Number of frequencies, magnitudes and phases must be same and >= 2.\n");
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < s->nb_freq; i++) {
        if (i == 0 && s->freq_points_opt[i] != 0.f) {
            av_log(ctx, AV_LOG_ERROR, "First frequency must be 0.\n");
            return AVERROR(EINVAL);
        }

        if (i == s->nb_freq - 1 && s->freq_points_opt[i] != 1.f) {
            av_log(ctx, AV_LOG_ERROR, "Last frequency must be 1.\n");
            return AVERROR(EINVAL);
        }

        if (i && s->freq_points_opt[i] < s->freq_points_opt[i-1]) {
            av_log(ctx, AV_LOG_ERROR, "Frequencies must be in increasing order.\n");
            return AVERROR(EINVAL);
        }
    }

    fft_size = 1 << (av_log2(s->nb_taps) + 1);
    s->complexf = av_calloc(fft_size * 2, sizeof(*s->complexf));
    if (!s->complexf)
        return AVERROR(ENOMEM);

    scale = 1.f / fft_size;
    ret = av_tx_init(&s->tx_ctx, &s->tx_fn, AV_TX_FLOAT_RDFT, 1, fft_size, &scale, 0);
    if (ret < 0)
        return ret;

    s->taps = av_calloc(s->nb_taps, sizeof(*s->taps));
    if (!s->taps)
        return AVERROR(ENOMEM);

    s->win = av_calloc(s->nb_taps, sizeof(*s->win));
    if (!s->win)
        return AVERROR(ENOMEM);

    generate_window_func(s->win, s->nb_taps, s->win_func, &overlap);

    lininterp(s->complexf, s->freq_points_opt, s->magnitude_opt, s->phase_opt, s->nb_freq, fft_size / 2);

    s->tx_fn(s->tx_ctx, s->complexf + fft_size, s->complexf, sizeof(*s->complexf));

    middle = s->nb_taps / 2;
    output = (float *)(s->complexf + fft_size);

    for (int i = 0; i <= middle; i++) {
        s->taps[         i] = output[middle - i] * s->win[i];
        s->taps[middle + i] = output[i] * s->win[middle + i];
    }

    s->pts = 0;

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    AudioFIRSourceContext *s = ctx->priv;
    AVFrame *frame;
    int nb_samples;

    if (!ff_outlink_frame_wanted(outlink))
        return FFERROR_NOT_READY;

    nb_samples = FFMIN(s->nb_samples, s->nb_taps - s->pts);
    if (nb_samples <= 0) {
        ff_outlink_set_status(outlink, AVERROR_EOF, s->pts);
        return 0;
    }

    if (!(frame = ff_get_audio_buffer(outlink, nb_samples)))
        return AVERROR(ENOMEM);

    memcpy(frame->data[0], s->taps + s->pts, nb_samples * sizeof(float));

    frame->pts = s->pts;
    s->pts    += nb_samples;
    return ff_filter_frame(outlink, frame);
}

static const AVFilterPad afirsrc_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_output,
    },
};

const FFFilter ff_asrc_afirsrc = {
    .p.name        = "afirsrc",
    .p.description = NULL_IF_CONFIG_SMALL("Generate a FIR coefficients audio stream."),
    .p.priv_class  = &afirsrc_class,
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    .priv_size     = sizeof(AudioFIRSourceContext),
    FILTER_OUTPUTS(afirsrc_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};

#define DEFAULT_BANDS "25 40 63 100 160 250 400 630 1000 1600 2500 4000 6300 10000 16000 24000"
#define DEFAULT_GAINS "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0"

static const AVOptionArrayDef def_bands = {.def=DEFAULT_BANDS,.size_min=1,.sep=' '};
static const AVOptionArrayDef def_gains = {.def=DEFAULT_GAINS,.size_min=1,.sep=' '};

typedef struct EqPreset {
    char name[16];
    float gains[16];
} EqPreset;

static const EqPreset eq_presets[] = {
    { "flat",          { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { "acoustic",      { 5.0, 4.5, 4.0, 3.5, 1.5, 1.0, 1.5, 1.5, 2.0, 3.0, 3.5, 4.0, 3.7, 3.0, 3.0 } },
    { "bass",          { 10.0, 8.8, 8.5, 6.5, 2.5, 1.5, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { "beats",         { -5.5, -5.0, -4.5, -4.2, -3.5, -3.0, -1.9, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { "classic",       { -0.3, 0.3, -3.5, -9.0, -1.0, 0.0, 1.8, 2.1, 0.0, 0.0, 0.0, 4.4, 9.0, 9.0, 9.0 } },
    { "clear",         { 3.5, 5.5, 6.5, 9.5, 8.0, 6.5, 3.5, 2.5, 1.3, 5.0, 7.0, 9.0, 10.0, 11.0, 9.0 } },
    { "deep bass",     { 12.0, 8.0, 0.0, -6.7, -12.0, -9.0, -3.5, -3.5, -6.1, 0.0, -3.0, -5.0, 0.0, 1.2, 3.0 } },
    { "dubstep",       { 12.0, 10.0, 0.5, -1.0, -3.0, -5.0, -5.0, -4.8, -4.5, -2.5, -1.0, 0.0, -2.5, -2.5, 0.0 } },
    { "electronic",    { 4.0, 4.0, 3.5, 1.0, 0.0, -0.5, -2.0, 0.0, 2.0, 0.0, 0.0, 1.0, 3.0, 4.0, 4.5 } },
    { "hardstyle",     { 6.1, 7.0, 12.0, 6.1, -5.0, -12.0, -2.5, 3.0, 6.5, 0.0, -2.2, -4.5, -6.1, -9.2, -10.0 } },
    { "hip-hop",       { 4.5, 4.3, 4.0, 2.5, 1.5, 3.0, -1.0, -1.5, -1.5, 1.5, 0.0, -1.0, 0.0, 1.5, 3.0 } },
    { "jazz",          { 0.0, 0.0, 0.0, 2.0, 4.0, 5.9, -5.9, -4.5, -2.5, 2.5, 1.0, -0.8, -0.8, -0.8, -0.8 } },
    { "metal",         { 10.5, 10.5, 7.5, 0.0, 2.0, 5.5, 0.0, 0.0, 0.0, 6.1, 0.0, 0.0, 6.1, 10.0, 12.0 } },
    { "movie",         { 3.0, 3.0, 6.1, 8.5, 9.0, 7.0, 6.1, 6.1, 5.0, 8.0, 3.5, 3.5, 8.0, 10.0, 8.0 } },
    { "pop",           { 0.0, 0.0, 0.0, 0.0, 0.0, 1.3, 2.0, 2.5, 5.0, -1.5, -2.0, -3.0, -3.0, -3.0, -3.0 } },
    { "r&b",           { 3.0, 3.0, 7.0, 6.1, 4.5, 1.5, -1.5, -2.0, -1.5, 2.0, 2.5, 3.0, 3.5, 3.8, 4.0 } },
    { "rock",          { 0.0, 0.0, 0.0, 3.0, 3.0, -10.0, -4.0, -1.0, 0.8, 3.0, 3.0, 3.0, 3.0, 3.0, 3.0 } },
    { "vocal booster", { -1.5, -2.0, -3.0, -3.0, -0.5, 1.5, 3.5, 3.5, 3.5, 3.0, 2.0, 1.5, 0.0, 0.0, -1.5 } },
};

static const AVOption afireqsrc_options[] = {
    { "preset","set equalizer preset", OFFSET(preset), AV_OPT_TYPE_INT, {.i64=0}, -1, FF_ARRAY_ELEMS(eq_presets)-1, FLAGS, .unit = "preset" },
    { "p",     "set equalizer preset", OFFSET(preset), AV_OPT_TYPE_INT, {.i64=0}, -1, FF_ARRAY_ELEMS(eq_presets)-1, FLAGS, .unit = "preset" },
    { "custom",            NULL, 0, AV_OPT_TYPE_CONST, {.i64=-1}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 0].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 0}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 1].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 1}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 2].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 2}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 3].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 3}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 4].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 4}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 5].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 5}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 6].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 6}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 7].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 7}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 8].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 8}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[ 9].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64= 9}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[10].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=10}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[11].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=11}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[12].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=12}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[13].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=13}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[14].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=14}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[15].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=15}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[16].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=16}, 0, 0, FLAGS, .unit = "preset" },
    { eq_presets[17].name, NULL, 0, AV_OPT_TYPE_CONST, {.i64=17}, 0, 0, FLAGS, .unit = "preset" },
    { "gains", "set gain values per band", OFFSET(magnitude_opt), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_gains}, INT_MIN, INT_MAX, FLAGS },
    { "g",     "set gain values per band", OFFSET(magnitude_opt), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_gains}, INT_MIN, INT_MAX, FLAGS },
    { "bands", "set central frequency values per band", OFFSET(freq_points_opt), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_bands}, 0, INT_MAX, FLAGS },
    { "b",     "set central frequency values per band", OFFSET(freq_points_opt), AV_OPT_TYPE_FLOAT|AR, {.arr=&def_bands}, 0, INT_MAX, FLAGS },
    { "taps", "set number of taps", OFFSET(nb_taps), AV_OPT_TYPE_INT, {.i64=4096}, 16, UINT16_MAX, FLAGS },
    { "t",    "set number of taps", OFFSET(nb_taps), AV_OPT_TYPE_INT, {.i64=4096}, 16, UINT16_MAX, FLAGS },
    { "sample_rate", "set sample rate", OFFSET(sample_rate), AV_OPT_TYPE_INT, {.i64=44100},  1, INT_MAX,    FLAGS },
    { "r",           "set sample rate", OFFSET(sample_rate), AV_OPT_TYPE_INT, {.i64=44100},  1, INT_MAX,    FLAGS },
    { "nb_samples", "set the number of samples per requested frame", OFFSET(nb_samples), AV_OPT_TYPE_INT, {.i64 = 1024}, 1, INT_MAX, FLAGS },
    { "n",          "set the number of samples per requested frame", OFFSET(nb_samples), AV_OPT_TYPE_INT, {.i64 = 1024}, 1, INT_MAX, FLAGS },
    { "interp","set the interpolation", OFFSET(interp), AV_OPT_TYPE_INT, {.i64=0}, 0, NB_INTERP-1, FLAGS, .unit = "interp" },
    { "i",     "set the interpolation", OFFSET(interp), AV_OPT_TYPE_INT, {.i64=0}, 0, NB_INTERP-1, FLAGS, .unit = "interp" },
    { "linear", NULL, 0, AV_OPT_TYPE_CONST, {.i64=INTERP_LINEAR}, 0, 0, FLAGS, .unit = "interp" },
    { "cubic",  NULL, 0, AV_OPT_TYPE_CONST, {.i64=INTERP_CUBIC},  0, 0, FLAGS, .unit = "interp" },
    { "spline", NULL, 0, AV_OPT_TYPE_CONST, {.i64=INTERP_SPLINE}, 0, 0, FLAGS, .unit = "interp" },
    { "phase","set the phase", OFFSET(phaset), AV_OPT_TYPE_INT, {.i64=1}, 0, 1, FLAGS, .unit = "phase" },
    { "h",    "set the phase", OFFSET(phaset), AV_OPT_TYPE_INT, {.i64=1}, 0, 1, FLAGS, .unit = "phase" },
    { "linear", "linear phase",  0, AV_OPT_TYPE_CONST, {.i64=0}, 0, 0, FLAGS, .unit = "phase" },
    { "min",    "minimum phase", 0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, FLAGS, .unit = "phase" },
    {NULL}
};

AVFILTER_DEFINE_CLASS(afireqsrc);

static void eq_interp(AVComplexFloat *complexf,
                      const float *freq,
                      const float *magnitude,
                      int m, int interp, int minterp,
                      const float factor)
{
    for (int i = 0; i < minterp; i++) {
        for (int j = 0; j < m; j++) {
            const float x = factor * i;

            if (x <= freq[j+1]) {
                float g = 1.f;

                if (interp == INTERP_LINEAR) {
                    const float d  = freq[j+1] - freq[j];
                    const float d0 = x - freq[j];
                    const float d1 = freq[j+1] - x;
                    const float g0 = magnitude[j];
                    const float g1 = magnitude[j+1];

                    if (d0 && d1) {
                        g = (d0 * g1 + d1 * g0) / d;
                    } else if (d0) {
                        g = g1;
                    } else {
                        g = g0;
                    }
                } else if (interp == INTERP_CUBIC) {
                    if (x <= freq[j]) {
                        g = magnitude[j];
                    } else {
                        float x1, x2, x3;
                        float a, b, c, d;
                        float m0, m1, m2, msum;
                        const float unit = freq[j+1] - freq[j];

                        m0 = j != 0 ? unit * (magnitude[j] - magnitude[j-1]) / (freq[j] - freq[j-1]) : 0;
                        m1 = magnitude[j+1] - magnitude[j];
                        m2 = j != minterp - 1 ? unit * (magnitude[j+2] - magnitude[j+1]) / (freq[j+2] - freq[j+1]) : 0;

                        msum = fabsf(m0) + fabsf(m1);
                        m0 = msum > 0.f ? (fabsf(m0) * m1 + fabsf(m1) * m0) / msum : 0.f;
                        msum = fabsf(m1) + fabsf(m2);
                        m1 = msum > 0.f ? (fabsf(m1) * m2 + fabsf(m2) * m1) / msum : 0.f;

                        d = magnitude[j];
                        c = m0;
                        b = 3.f * magnitude[j+1] - m1 - 2.f * c - 3.f * d;
                        a = magnitude[j+1] - b - c - d;

                        x1 = (x - freq[j]) / unit;
                        x2 = x1 * x1;
                        x3 = x2 * x1;

                        g = a * x3 + b * x2 + c * x1 + d;
                    }
                } else if (interp == INTERP_SPLINE) {
                    const float d = freq[j+1] - freq[j];
                    const float u = (x - freq[j]) / d;
                    const float c0 = magnitude[j];
                    const float c1 = 1/2.f*(magnitude[j+1]-magnitude[j-1]);
                    const float c2 = magnitude[j-1] - 5/2.f*magnitude[j] + 2*magnitude[j+1] - 1/2.f*magnitude[j+2];
                    const float c3 = 1/2.f*(magnitude[j+2]-magnitude[j-1]) + 3/2.f*(magnitude[j]-magnitude[j+1]);

                    g = ((c3 * u + c2) * u + c1) * u + c0;
                }

                complexf[i].re = g;
                complexf[i].im = 0;
                complexf[minterp * 2 - i - 1].re = g;
                complexf[minterp * 2 - i - 1].im = 0;

                break;
            }
        }
    }
}

static av_cold int config_eq_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AudioFIRSourceContext *s = ctx->priv;
    int fft_size, middle, asize, ret;
    float scale, factor;

    if (s->preset >= 0) {
        const int nb_magnitude = FF_ARRAY_ELEMS(eq_presets[s->preset].gains);

        if (s->nb_freq != nb_magnitude) {
            av_log(ctx, AV_LOG_ERROR, "Number of bands and gains must be same and >= 2.\n");
            return AVERROR(EINVAL);
        }

        s->freq = av_calloc(s->nb_freq+1, sizeof(*s->freq));
        s->magnitude = av_calloc(nb_magnitude+1, sizeof(*s->magnitude));
        if (!s->magnitude || !s->freq)
            return AVERROR(ENOMEM);

        memcpy(s->freq, s->freq_points_opt, sizeof(*s->freq) * s->nb_freq);
        memcpy(s->magnitude, eq_presets[s->preset].gains, sizeof(*s->magnitude) * nb_magnitude);
    } else {
        if (s->nb_freq != s->nb_magnitude || s->nb_freq < 2) {
            av_log(ctx, AV_LOG_ERROR, "Number of bands and gains must be same and >= 2.\n");
            return AVERROR(EINVAL);
        }

        s->freq = av_calloc(s->nb_magnitude+1, sizeof(*s->freq));
        s->magnitude = av_calloc(s->nb_magnitude+1, sizeof(*s->magnitude));
        if (!s->magnitude || !s->freq)
            return AVERROR(ENOMEM);

        memcpy(s->freq, s->freq_points_opt, sizeof(*s->freq) * s->nb_magnitude);
        memcpy(s->magnitude, s->magnitude_opt, sizeof(*s->magnitude) * s->nb_magnitude);
    }

    s->freq[s->nb_freq] = outlink->sample_rate * 0.5f;
    s->magnitude[s->nb_freq] = s->magnitude[s->nb_freq-1];

    fft_size = s->nb_taps * 2;
    factor = FFMIN(outlink->sample_rate * 0.5f, s->freq[s->nb_freq - 1]) / (float)s->nb_taps;
    asize = FFALIGN(fft_size, av_cpu_max_align());
    s->complexf = av_calloc(asize * 2, sizeof(*s->complexf));
    if (!s->complexf)
        return AVERROR(ENOMEM);

    scale = 1.f;
    ret = av_tx_init(&s->itx_ctx, &s->itx_fn, AV_TX_FLOAT_FFT, 1, fft_size, &scale, 0);
    if (ret < 0)
        return ret;

    s->taps = av_calloc(s->nb_taps, sizeof(*s->taps));
    if (!s->taps)
        return AVERROR(ENOMEM);

    eq_interp(s->complexf, s->freq, s->magnitude, s->nb_freq, s->interp, s->nb_taps, factor);

    for (int i = 0; i < fft_size; i++)
        s->complexf[i].re = ff_exp10f(s->complexf[i].re / 20.f);

    if (s->phaset) {
        const float threshold = powf(10.f, -100.f / 20.f);
        const float logt = logf(threshold);

        scale = 1.f;
        ret = av_tx_init(&s->tx_ctx, &s->tx_fn, AV_TX_FLOAT_FFT, 0, fft_size, &scale, 0);
        if (ret < 0)
            return ret;

        for (int i = 0; i < fft_size; i++)
            s->complexf[i].re = s->complexf[i].re < threshold ? logt : logf(s->complexf[i].re);

        s->itx_fn(s->itx_ctx, s->complexf + asize, s->complexf, sizeof(AVComplexFloat));
        for (int i = 0; i < fft_size; i++) {
            s->complexf[i + asize].re /= fft_size;
            s->complexf[i + asize].im /= fft_size;
        }

        for (int i = 1; i < s->nb_taps; i++) {
            s->complexf[asize + i].re += s->complexf[asize + fft_size - i].re;
            s->complexf[asize + i].im -= s->complexf[asize + fft_size - i].im;
            s->complexf[asize + fft_size - i].re = 0.f;
            s->complexf[asize + fft_size - i].im = 0.f;
        }
        s->complexf[asize + s->nb_taps - 1].im *= -1.f;

        s->tx_fn(s->tx_ctx, s->complexf, s->complexf + asize, sizeof(AVComplexFloat));

        for (int i = 0; i < fft_size; i++) {
            float eR = expf(s->complexf[i].re);

            s->complexf[i].re = eR * cosf(s->complexf[i].im);
            s->complexf[i].im = eR * sinf(s->complexf[i].im);
        }

        s->itx_fn(s->itx_ctx, s->complexf + asize, s->complexf, sizeof(AVComplexFloat));

        for (int i = 0; i < s->nb_taps; i++)
            s->taps[i] = s->complexf[i + asize].re / fft_size;
    } else {
        s->itx_fn(s->itx_ctx, s->complexf + asize, s->complexf, sizeof(AVComplexFloat));

        middle = s->nb_taps / 2;
        for (int i = 0; i < middle; i++) {
            s->taps[middle - i] = s->complexf[i + asize].re / fft_size;
            s->taps[middle + i] = s->complexf[i + asize].re / fft_size;
        }
    }

    s->pts = 0;

    return 0;
}

static const AVFilterPad afireqsrc_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = config_eq_output,
    },
};

const FFFilter ff_asrc_afireqsrc = {
    .p.name        = "afireqsrc",
    .p.description = NULL_IF_CONFIG_SMALL("Generate a FIR equalizer coefficients audio stream."),
    .p.priv_class  = &afireqsrc_class,
    .uninit        = uninit,
    .activate      = activate,
    .priv_size     = sizeof(AudioFIRSourceContext),
    FILTER_OUTPUTS(afireqsrc_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
