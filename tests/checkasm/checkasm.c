/*
 * Assembly testing and benchmarking tool
 * Copyright (c) 2015 Henrik Gramner
 * Copyright (c) 2008 Loren Merritt
 *
 * This file is part of Librempeg.
 *
 * Librempeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Librempeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "config_components.h"

#ifndef _GNU_SOURCE
# define _GNU_SOURCE // for syscall (performance monitoring API), strsignal()
#endif

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "checkasm.h"
#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/cpu.h"
#include "libavutil/intfloat.h"
#include "libavutil/random_seed.h"

#if HAVE_IO_H
#include <io.h>
#endif
#if HAVE_PRCTL
#include <sys/prctl.h>
#endif

#if defined(_WIN32) && !defined(SIGBUS)
/* non-standard, use the same value as mingw-w64 */
#define SIGBUS 10
#endif

#if HAVE_SETCONSOLETEXTATTRIBUTE && HAVE_GETSTDHANDLE
#include <windows.h>
#define COLOR_RED    FOREGROUND_RED
#define COLOR_GREEN  FOREGROUND_GREEN
#define COLOR_YELLOW (FOREGROUND_RED|FOREGROUND_GREEN)
#else
#define COLOR_RED    1
#define COLOR_GREEN  2
#define COLOR_YELLOW 3
#endif

#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#if !HAVE_ISATTY
#define isatty(fd) 1
#endif

#if ARCH_AARCH64
#include "libavutil/aarch64/cpu.h"
#elif ARCH_RISCV
#include "libavutil/riscv/cpu.h"
#endif

#if ARCH_ARM && HAVE_ARMV5TE_EXTERNAL
#include "libavutil/arm/cpu.h"

void (*checkasm_checked_call)(void *func, int dummy, ...) = checkasm_checked_call_novfp;
#endif

/* Trade-off between speed and accuracy */
uint64_t bench_runs = 1U << 10;

/* List of tests to invoke */
static const struct {
    const char *name;
    void (*func)(void);
} tests[] = {
#if CONFIG_AVCODEC
    #if CONFIG_AAC_DECODER
        { "aacpsdsp", checkasm_check_aacpsdsp },
        { "sbrdsp",   checkasm_check_sbrdsp },
    #endif
    #if CONFIG_AAC_ENCODER
        { "aacencdsp", checkasm_check_aacencdsp },
    #endif
    #if CONFIG_AC3DSP
        { "ac3dsp", checkasm_check_ac3dsp },
    #endif
    #if CONFIG_ALAC_DECODER
        { "alacdsp", checkasm_check_alacdsp },
    #endif
    #if CONFIG_APV_DECODER
        { "apv_dsp", checkasm_check_apv_dsp },
    #endif
    #if CONFIG_AUDIODSP
        { "audiodsp", checkasm_check_audiodsp },
    #endif
    #if CONFIG_BLOCKDSP
        { "blockdsp", checkasm_check_blockdsp },
    #endif
    #if CONFIG_BSWAPDSP
        { "bswapdsp", checkasm_check_bswapdsp },
    #endif
    #if CONFIG_DCA_DECODER
        { "synth_filter", checkasm_check_synth_filter },
    #endif
    #if CONFIG_DIRAC_DECODER
        { "diracdsp", checkasm_check_diracdsp },
    #endif
    #if CONFIG_EXR_DECODER
        { "exrdsp", checkasm_check_exrdsp },
    #endif
    #if CONFIG_FDCTDSP
        { "fdctdsp", checkasm_check_fdctdsp },
    #endif
    #if CONFIG_FLAC_DECODER
        { "flacdsp", checkasm_check_flacdsp },
    #endif
    #if CONFIG_FMTCONVERT
        { "fmtconvert", checkasm_check_fmtconvert },
    #endif
    #if CONFIG_G722DSP
        { "g722dsp", checkasm_check_g722dsp },
    #endif
    #if CONFIG_H263DSP
        { "h263dsp", checkasm_check_h263dsp },
    #endif
    #if CONFIG_H264CHROMA
        { "h264chroma", checkasm_check_h264chroma },
    #endif
    #if CONFIG_H264DSP
        { "h264dsp", checkasm_check_h264dsp },
    #endif
    #if CONFIG_H264PRED
        { "h264pred", checkasm_check_h264pred },
    #endif
    #if CONFIG_H264QPEL
        { "h264qpel", checkasm_check_h264qpel },
    #endif
    #if CONFIG_HEVC_DECODER
        { "hevc_add_res", checkasm_check_hevc_add_res },
        { "hevc_deblock", checkasm_check_hevc_deblock },
        { "hevc_idct", checkasm_check_hevc_idct },
        { "hevc_pel", checkasm_check_hevc_pel },
        { "hevc_sao", checkasm_check_hevc_sao },
    #endif
    #if CONFIG_HUFFYUV_DECODER
        { "huffyuvdsp", checkasm_check_huffyuvdsp },
    #endif
    #if CONFIG_IDCTDSP
        { "idctdsp", checkasm_check_idctdsp },
    #endif
    #if CONFIG_JPEG2000_DECODER
        { "jpeg2000dsp", checkasm_check_jpeg2000dsp },
    #endif
    #if CONFIG_LLAUDDSP
        { "llauddsp", checkasm_check_llauddsp },
    #endif
    #if CONFIG_HUFFYUVDSP
        { "llviddsp", checkasm_check_llviddsp },
    #endif
    #if CONFIG_LLVIDENCDSP
        { "llviddspenc", checkasm_check_llviddspenc },
    #endif
    #if CONFIG_LPC
        { "lpc", checkasm_check_lpc },
    #endif
    #if CONFIG_ME_CMP
        { "motion", checkasm_check_motion },
    #endif
    #if CONFIG_MPEGVIDEOENCDSP
        { "mpegvideoencdsp", checkasm_check_mpegvideoencdsp },
    #endif
    #if CONFIG_OPUS_DECODER
        { "opusdsp", checkasm_check_opusdsp },
    #endif
    #if CONFIG_PIXBLOCKDSP
        { "pixblockdsp", checkasm_check_pixblockdsp },
    #endif
    #if CONFIG_RV34DSP
        { "rv34dsp", checkasm_check_rv34dsp },
    #endif
    #if CONFIG_RV40_DECODER
        { "rv40dsp", checkasm_check_rv40dsp },
    #endif
    #if CONFIG_SVQ1_ENCODER
        { "svq1enc", checkasm_check_svq1enc },
    #endif
    #if CONFIG_TAK_DECODER
        { "takdsp", checkasm_check_takdsp },
    #endif
    #if CONFIG_UTVIDEO_DECODER
        { "utvideodsp", checkasm_check_utvideodsp },
    #endif
    #if CONFIG_V210_DECODER
        { "v210dec", checkasm_check_v210dec },
    #endif
    #if CONFIG_V210_ENCODER
        { "v210enc", checkasm_check_v210enc },
    #endif
    #if CONFIG_VC1DSP
        { "vc1dsp", checkasm_check_vc1dsp },
    #endif
    #if CONFIG_VP8DSP
        { "vp8dsp", checkasm_check_vp8dsp },
    #endif
    #if CONFIG_VP9_DECODER
        { "vp9dsp", checkasm_check_vp9dsp },
    #endif
    #if CONFIG_VIDEODSP
        { "videodsp", checkasm_check_videodsp },
    #endif
    #if CONFIG_VORBIS_DECODER
        { "vorbisdsp", checkasm_check_vorbisdsp },
    #endif
    #if CONFIG_VVC_DECODER
        { "vvc_alf", checkasm_check_vvc_alf },
        { "vvc_mc",  checkasm_check_vvc_mc  },
        { "vvc_sao", checkasm_check_vvc_sao },
    #endif
#endif
#if CONFIG_AVFILTER
    #if CONFIG_SCENE_SAD
        { "scene_sad", checkasm_check_scene_sad },
    #endif
    #if CONFIG_AFIR_FILTER
        { "af_afir", checkasm_check_afir },
    #endif
    #if CONFIG_BLACKDETECT_FILTER
        { "vf_blackdetect", checkasm_check_blackdetect },
    #endif
    #if CONFIG_BLEND_FILTER
        { "vf_blend", checkasm_check_blend },
    #endif
    #if CONFIG_BWDIF_FILTER
        { "vf_bwdif", checkasm_check_vf_bwdif },
    #endif
    #if CONFIG_COLORDETECT_FILTER
        { "vf_colordetect", checkasm_check_colordetect },
    #endif
    #if CONFIG_COLORSPACE_FILTER
        { "vf_colorspace", checkasm_check_colorspace },
    #endif
    #if CONFIG_EQ_FILTER
        { "vf_eq", checkasm_check_vf_eq },
    #endif
    #if CONFIG_GBLUR_FILTER
        { "vf_gblur", checkasm_check_vf_gblur },
    #endif
    #if CONFIG_HFLIP_FILTER
        { "vf_hflip", checkasm_check_vf_hflip },
    #endif
    #if CONFIG_NLMEANS_FILTER
        { "vf_nlmeans", checkasm_check_nlmeans },
    #endif
    #if CONFIG_THRESHOLD_FILTER
        { "vf_threshold", checkasm_check_vf_threshold },
    #endif
    #if CONFIG_SOBEL_FILTER
        { "vf_sobel", checkasm_check_vf_sobel },
    #endif
#endif
#if CONFIG_SWSCALE
    { "sw_gbrp", checkasm_check_sw_gbrp },
    { "sw_range_convert", checkasm_check_sw_range_convert },
    { "sw_rgb", checkasm_check_sw_rgb },
    { "sw_scale", checkasm_check_sw_scale },
    { "sw_yuv2rgb", checkasm_check_sw_yuv2rgb },
    { "sw_yuv2yuv", checkasm_check_sw_yuv2yuv },
#endif
#if CONFIG_AVUTIL
        { "aes",       checkasm_check_aes },
        { "fixed_dsp", checkasm_check_fixed_dsp },
        { "float_dsp", checkasm_check_float_dsp },
        { "lls",       checkasm_check_lls },
        { "av_tx",     checkasm_check_av_tx },
#endif
    { NULL }
};

/* List of cpu flags to check */
static const struct {
    const char *name;
    const char *suffix;
    int flag;
} cpus[] = {
#if   ARCH_AARCH64
    { "ARMV8",    "armv8",    AV_CPU_FLAG_ARMV8 },
    { "NEON",     "neon",     AV_CPU_FLAG_NEON },
    { "DOTPROD",  "dotprod",  AV_CPU_FLAG_DOTPROD },
    { "I8MM",     "i8mm",     AV_CPU_FLAG_I8MM },
    { "SVE",      "sve",      AV_CPU_FLAG_SVE },
    { "SVE2",     "sve2",     AV_CPU_FLAG_SVE2 },
#elif ARCH_ARM
    { "ARMV5TE",  "armv5te",  AV_CPU_FLAG_ARMV5TE },
    { "ARMV6",    "armv6",    AV_CPU_FLAG_ARMV6 },
    { "ARMV6T2",  "armv6t2",  AV_CPU_FLAG_ARMV6T2 },
    { "VFP",      "vfp",      AV_CPU_FLAG_VFP },
    { "VFP_VM",   "vfp_vm",   AV_CPU_FLAG_VFP_VM },
    { "VFPV3",    "vfp3",     AV_CPU_FLAG_VFPV3 },
    { "NEON",     "neon",     AV_CPU_FLAG_NEON },
#elif ARCH_PPC
    { "ALTIVEC",  "altivec",  AV_CPU_FLAG_ALTIVEC },
    { "VSX",      "vsx",      AV_CPU_FLAG_VSX },
    { "POWER8",   "power8",   AV_CPU_FLAG_POWER8 },
#elif ARCH_RISCV
    { "RVI",      "rvi",      AV_CPU_FLAG_RVI },
    { "misaligned", "misaligned", AV_CPU_FLAG_RV_MISALIGNED },
    { "RV_zbb",   "rvb_b",    AV_CPU_FLAG_RVB_BASIC },
    { "RVB",      "rvb",      AV_CPU_FLAG_RVB },
    { "RV_zve32x","rvv_i32",  AV_CPU_FLAG_RVV_I32 },
    { "RV_zve32f","rvv_f32",  AV_CPU_FLAG_RVV_F32 },
    { "RV_zve64x","rvv_i64",  AV_CPU_FLAG_RVV_I64 },
    { "RV_zve64d","rvv_f64",  AV_CPU_FLAG_RVV_F64 },
    { "RV_zvbb",  "rv_zvbb",  AV_CPU_FLAG_RV_ZVBB },
#elif ARCH_MIPS
    { "MMI",      "mmi",      AV_CPU_FLAG_MMI },
    { "MSA",      "msa",      AV_CPU_FLAG_MSA },
#elif ARCH_X86
    { "MMX",        "mmx",       AV_CPU_FLAG_MMX|AV_CPU_FLAG_CMOV },
    { "MMXEXT",     "mmxext",    AV_CPU_FLAG_MMXEXT },
    { "3DNOW",      "3dnow",     AV_CPU_FLAG_3DNOW },
    { "3DNOWEXT",   "3dnowext",  AV_CPU_FLAG_3DNOWEXT },
    { "SSE",        "sse",       AV_CPU_FLAG_SSE },
    { "SSE2",       "sse2",      AV_CPU_FLAG_SSE2|AV_CPU_FLAG_SSE2SLOW },
    { "SSE3",       "sse3",      AV_CPU_FLAG_SSE3|AV_CPU_FLAG_SSE3SLOW },
    { "SSSE3",      "ssse3",     AV_CPU_FLAG_SSSE3|AV_CPU_FLAG_ATOM },
    { "SSE4.1",     "sse4",      AV_CPU_FLAG_SSE4 },
    { "SSE4.2",     "sse42",     AV_CPU_FLAG_SSE42 },
    { "AES-NI",     "aesni",     AV_CPU_FLAG_AESNI },
    { "AVX",        "avx",       AV_CPU_FLAG_AVX },
    { "XOP",        "xop",       AV_CPU_FLAG_XOP },
    { "FMA3",       "fma3",      AV_CPU_FLAG_FMA3 },
    { "FMA4",       "fma4",      AV_CPU_FLAG_FMA4 },
    { "AVX2",       "avx2",      AV_CPU_FLAG_AVX2 },
    { "AVX-512",    "avx512",    AV_CPU_FLAG_AVX512 },
    { "AVX-512ICL", "avx512icl", AV_CPU_FLAG_AVX512ICL },
#elif ARCH_LOONGARCH
    { "LSX",      "lsx",      AV_CPU_FLAG_LSX },
    { "LASX",     "lasx",     AV_CPU_FLAG_LASX },
#elif ARCH_WASM
    { "SIMD128",    "simd128",  AV_CPU_FLAG_SIMD128 },
#endif
    { NULL }
};

typedef struct CheckasmFuncVersion {
    struct CheckasmFuncVersion *next;
    void *func;
    int ok;
    int cpu;
    CheckasmPerf perf;
} CheckasmFuncVersion;

/* Binary search tree node */
typedef struct CheckasmFunc {
    struct CheckasmFunc *child[2];
    CheckasmFuncVersion versions;
    uint8_t color; /* 0 = red, 1 = black */
    char name[1];
} CheckasmFunc;

/* Internal state */
static struct {
    CheckasmFunc *funcs;
    CheckasmFunc *current_func;
    CheckasmFuncVersion *current_func_ver;
    const char *current_test_name;
    const char *bench_pattern;
    int bench_pattern_len;
    int num_checked;
    int num_failed;

    /* perf */
    int nop_time;
    int sysfd;

    int cpu_flag;
    const char *cpu_flag_name;
    const char *test_pattern;
    int verbose;
    int csv;
    int tsv;
    volatile sig_atomic_t catch_signals;
} state;

/* PRNG state */
AVLFG checkasm_lfg;

/* float compare support code */
static int is_negative(union av_intfloat32 u)
{
    return u.i >> 31;
}

int float_near_ulp(float a, float b, unsigned max_ulp)
{
    union av_intfloat32 x, y;

    x.f = a;
    y.f = b;

    if (is_negative(x) != is_negative(y)) {
        // handle -0.0 == +0.0
        return a == b;
    }

    if (llabs((int64_t)x.i - y.i) <= max_ulp)
        return 1;

    return 0;
}

int float_near_ulp_array(const float *a, const float *b, unsigned max_ulp,
                         unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        if (!float_near_ulp(a[i], b[i], max_ulp))
            return 0;
    }
    return 1;
}

int float_near_abs_eps(float a, float b, float eps)
{
    float abs_diff = fabsf(a - b);
    if (abs_diff < eps)
        return 1;

    fprintf(stderr, "test failed comparing %g with %g (abs diff=%g with EPS=%g)\n", a, b, abs_diff, eps);

    return 0;
}

int float_near_abs_eps_array(const float *a, const float *b, float eps,
                         unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        if (!float_near_abs_eps(a[i], b[i], eps))
            return 0;
    }
    return 1;
}

int float_near_abs_eps_ulp(float a, float b, float eps, unsigned max_ulp)
{
    return float_near_ulp(a, b, max_ulp) || float_near_abs_eps(a, b, eps);
}

int float_near_abs_eps_array_ulp(const float *a, const float *b, float eps,
                         unsigned max_ulp, unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        if (!float_near_abs_eps_ulp(a[i], b[i], eps, max_ulp))
            return 0;
    }
    return 1;
}

int double_near_abs_eps(double a, double b, double eps)
{
    double abs_diff = fabs(a - b);

    return abs_diff < eps;
}

int double_near_abs_eps_array(const double *a, const double *b, double eps,
                              unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        if (!double_near_abs_eps(a[i], b[i], eps))
            return 0;
    }
    return 1;
}

/* Print colored text to stderr if the terminal supports it */
static void color_printf(int color, const char *fmt, ...)
{
    static int use_color = -1;
    va_list arg;

#if HAVE_SETCONSOLETEXTATTRIBUTE && HAVE_GETSTDHANDLE
    static HANDLE con;
    static WORD org_attributes;

    if (use_color < 0) {
        CONSOLE_SCREEN_BUFFER_INFO con_info;
        con = GetStdHandle(STD_ERROR_HANDLE);
        if (con && con != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(con, &con_info)) {
            org_attributes = con_info.wAttributes;
            use_color = 1;
        } else
            use_color = 0;
    }
    if (use_color)
        SetConsoleTextAttribute(con, (org_attributes & 0xfff0) | (color & 0x0f));
#else
    if (use_color < 0) {
        const char *term = getenv("TERM");
        use_color = term && strcmp(term, "dumb") && isatty(2);
    }
    if (use_color)
        fprintf(stderr, "\x1b[%d;3%dm", (color & 0x08) >> 3, color & 0x07);
#endif

    va_start(arg, fmt);
    vfprintf(stderr, fmt, arg);
    va_end(arg);

    if (use_color) {
#if HAVE_SETCONSOLETEXTATTRIBUTE && HAVE_GETSTDHANDLE
        SetConsoleTextAttribute(con, org_attributes);
#else
        fprintf(stderr, "\x1b[0m");
#endif
    }
}

/* Deallocate a tree */
static void destroy_func_tree(CheckasmFunc *f)
{
    if (f) {
        CheckasmFuncVersion *v = f->versions.next;
        while (v) {
            CheckasmFuncVersion *next = v->next;
            free(v);
            v = next;
        }

        destroy_func_tree(f->child[0]);
        destroy_func_tree(f->child[1]);
        free(f);
    }
}

/* Allocate a zero-initialized block, clean up and exit on failure */
static void *checkasm_malloc(size_t size)
{
    void *ptr = calloc(1, size);
    if (!ptr) {
        fprintf(stderr, "checkasm: malloc failed\n");
        destroy_func_tree(state.funcs);
        exit(1);
    }
    return ptr;
}

/* Get the suffix of the specified cpu flag */
static const char *cpu_suffix(int cpu)
{
    int i = FF_ARRAY_ELEMS(cpus);

    while (--i >= 0)
        if (cpu & cpus[i].flag)
            return cpus[i].suffix;

    return "c";
}

static int cmp_nop(const void *a, const void *b)
{
    return *(const uint16_t*)a - *(const uint16_t*)b;
}

/* Measure the overhead of the timing code (in decicycles) */
static int measure_nop_time(void)
{
    uint16_t nops[10000];
    int i, nop_sum = 0;
    av_unused const int sysfd = state.sysfd;

    uint64_t t = 0;
    for (i = 0; i < 10000; i++) {
        PERF_START(t);
        PERF_STOP(t);
        nops[i] = t;
    }

    qsort(nops, 10000, sizeof(uint16_t), cmp_nop);
    for (i = 2500; i < 7500; i++)
        nop_sum += nops[i];

    return nop_sum / 500;
}

static inline double avg_cycles_per_call(const CheckasmPerf *const p)
{
    if (p->iterations) {
        const double cycles = (double)(10 * p->cycles) / p->iterations - state.nop_time;
        if (cycles > 0.0)
            return cycles / 4.0; /* 4 calls per iteration */
    }
    return 0.0;
}

/* Print benchmark results */
static void print_benchs(CheckasmFunc *f)
{
    if (f) {
        CheckasmFuncVersion *v = &f->versions;
        const CheckasmPerf *p = &v->perf;
        const double baseline = avg_cycles_per_call(p);
        double decicycles;

        print_benchs(f->child[0]);

        do {
            if (p->iterations) {
                p = &v->perf;
                decicycles = avg_cycles_per_call(p);
                if (state.csv || state.tsv) {
                    const char sep = state.csv ? ',' : '\t';
                    printf("%s%c%s%c%.1f\n", f->name, sep,
                           cpu_suffix(v->cpu), sep,
                           decicycles / 10.0);
                } else {
                    const int pad_length = 10 + 50 -
                        printf("%s_%s:", f->name, cpu_suffix(v->cpu));
                    const double ratio = decicycles ?
                        baseline / decicycles : 0.0;
                    printf("%*.1f (%5.2fx)\n", FFMAX(pad_length, 0),
                        decicycles / 10.0, ratio);
                }
            }
        } while ((v = v->next));

        print_benchs(f->child[1]);
    }
}

/* ASCIIbetical sort except preserving natural order for numbers */
static int cmp_func_names(const char *a, const char *b)
{
    const char *start = a;
    int ascii_diff, digit_diff;

    for (; !(ascii_diff = *(const unsigned char*)a - *(const unsigned char*)b) && *a; a++, b++);
    for (; av_isdigit(*a) && av_isdigit(*b); a++, b++);

    if (a > start && av_isdigit(a[-1]) && (digit_diff = av_isdigit(*a) - av_isdigit(*b)))
        return digit_diff;

    return ascii_diff;
}

/* Perform a tree rotation in the specified direction and return the new root */
static CheckasmFunc *rotate_tree(CheckasmFunc *f, int dir)
{
    CheckasmFunc *r = f->child[dir^1];
    f->child[dir^1] = r->child[dir];
    r->child[dir] = f;
    r->color = f->color;
    f->color = 0;
    return r;
}

#define is_red(f) ((f) && !(f)->color)

/* Balance a left-leaning red-black tree at the specified node */
static void balance_tree(CheckasmFunc **root)
{
    CheckasmFunc *f = *root;

    if (is_red(f->child[0]) && is_red(f->child[1])) {
        f->color ^= 1;
        f->child[0]->color = f->child[1]->color = 1;
    }

    if (!is_red(f->child[0]) && is_red(f->child[1]))
        *root = rotate_tree(f, 0); /* Rotate left */
    else if (is_red(f->child[0]) && is_red(f->child[0]->child[0]))
        *root = rotate_tree(f, 1); /* Rotate right */
}

/* Get a node with the specified name, creating it if it doesn't exist */
static CheckasmFunc *get_func(CheckasmFunc **root, const char *name)
{
    CheckasmFunc *f = *root;

    if (f) {
        /* Search the tree for a matching node */
        int cmp = cmp_func_names(name, f->name);
        if (cmp) {
            f = get_func(&f->child[cmp > 0], name);

            /* Rebalance the tree on the way up if a new node was inserted */
            if (!f->versions.func)
                balance_tree(root);
        }
    } else {
        /* Allocate and insert a new node into the tree */
        int name_length = strlen(name);
        f = *root = checkasm_malloc(sizeof(CheckasmFunc) + name_length);
        memcpy(f->name, name, name_length + 1);
    }

    return f;
}

checkasm_context checkasm_context_buf;

/* Crash handling: attempt to catch crashes and handle them
 * gracefully instead of just aborting abruptly. */
#ifdef _WIN32
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
static LONG NTAPI signal_handler(EXCEPTION_POINTERS *e) {
    int s;

    if (!state.catch_signals)
        return EXCEPTION_CONTINUE_SEARCH;

    switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        s = SIGFPE;
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
        s = SIGILL;
        break;
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_STACK_OVERFLOW:
        s = SIGSEGV;
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        s = SIGBUS;
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    state.catch_signals = 0;
    checkasm_load_context(s);
    return EXCEPTION_CONTINUE_EXECUTION; /* never reached, but shuts up gcc */
}
#endif
#elif !defined(_WASI_EMULATED_SIGNAL)
static void signal_handler(int s);

static const struct sigaction signal_handler_act = {
    .sa_handler = signal_handler,
    .sa_flags = SA_RESETHAND,
};

static void signal_handler(int s) {
    if (state.catch_signals) {
        state.catch_signals = 0;
        sigaction(s, &signal_handler_act, NULL);
        checkasm_load_context(s);
    }
}
#endif

/* Compares a string with a wildcard pattern. */
static int wildstrcmp(const char *str, const char *pattern)
{
    const char *wild = strchr(pattern, '*');
    if (wild) {
        const size_t len = wild - pattern;
        if (strncmp(str, pattern, len)) return 1;
        while (*++wild == '*');
        if (!*wild) return 0;
        str += len;
        while (*str && wildstrcmp(str, wild)) str++;
        return !*str;
    }
    return strcmp(str, pattern);
}

/* Perform tests and benchmarks for the specified cpu flag if supported by the host */
static void check_cpu_flag(const char *name, int flag)
{
    int old_cpu_flag = state.cpu_flag;

    flag |= old_cpu_flag;
    av_force_cpu_flags(-1);
    state.cpu_flag = flag & av_get_cpu_flags();
    av_force_cpu_flags(state.cpu_flag);

    if (!flag || state.cpu_flag != old_cpu_flag) {
        int i;

        state.cpu_flag_name = name;
        for (i = 0; tests[i].func; i++) {
            if (state.test_pattern && wildstrcmp(tests[i].name, state.test_pattern))
                continue;
            state.current_test_name = tests[i].name;
            tests[i].func();
        }
    }
}

/* Print the name of the current CPU flag, but only do it once */
static void print_cpu_name(void)
{
    if (state.cpu_flag_name) {
        color_printf(COLOR_YELLOW, "%s:\n", state.cpu_flag_name);
        state.cpu_flag_name = NULL;
    }
}

#if CONFIG_LINUX_PERF
static int bench_init_linux(void)
{
    struct perf_event_attr attr = {
        .type           = PERF_TYPE_HARDWARE,
        .size           = sizeof(struct perf_event_attr),
        .config         = PERF_COUNT_HW_CPU_CYCLES,
        .disabled       = 1, // start counting only on demand
        .exclude_kernel = 1,
        .exclude_hv     = 1,
#if !ARCH_X86
        .exclude_guest  = 1,
#endif
    };

    fprintf(stderr, "benchmarking with Linux Perf Monitoring API\n");

    state.sysfd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
    if (state.sysfd == -1) {
        perror("perf_event_open");
        return -1;
    }
    return 0;
}
#elif CONFIG_MACOS_KPERF
static int bench_init_kperf(void)
{
    ff_kperf_init();
    return 0;
}
#else
static int bench_init_ffmpeg(void)
{
#ifdef AV_READ_TIME
    if (!checkasm_save_context()) {
        checkasm_set_signal_handler_state(1);
        AV_READ_TIME();
        checkasm_set_signal_handler_state(0);
    } else {
        fprintf(stderr, "checkasm: unable to execute platform specific timer\n");
        return -1;
    }
    fprintf(stderr, "benchmarking with native FFmpeg timers\n");
    return 0;
#else
    fprintf(stderr, "checkasm: --bench is not supported on your system\n");
    return -1;
#endif
}
#endif

static int bench_init(void)
{
#if CONFIG_LINUX_PERF
    int ret = bench_init_linux();
#elif CONFIG_MACOS_KPERF
    int ret = bench_init_kperf();
#else
    int ret = bench_init_ffmpeg();
#endif
    if (ret < 0)
        return ret;

    state.nop_time = measure_nop_time();
    fprintf(stderr, "nop: %d.%d\n", state.nop_time/10, state.nop_time%10);
    return 0;
}

static void bench_uninit(void)
{
#if CONFIG_LINUX_PERF
    close(state.sysfd);
#endif
}

static int usage(const char *path)
{
    fprintf(stderr,
            "Usage: %s [options...] [seed]\n"
            "  --test=<pattern> Run specific test.\n"
            "  --bench          Run benchmark.\n"
            "  --csv, --tsv     Output results in rows of comma or tab separated values.\n"
            "  --runs=<ptwo>    Manual number of benchmark iterations to run 2**<ptwo>.\n"
            "  --verbose        Increase verbosity.\n",
            path);
    return 1;
}

int main(int argc, char *argv[])
{
    unsigned int seed = av_get_random_seed();
    int i, ret = 0;
    char arch_info_buf[50] = "";

#ifdef _WIN32
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    AddVectoredExceptionHandler(0, signal_handler);
#endif
#elif !defined(_WASI_EMULATED_SIGNAL)
    sigaction(SIGBUS,  &signal_handler_act, NULL);
    sigaction(SIGFPE,  &signal_handler_act, NULL);
    sigaction(SIGILL,  &signal_handler_act, NULL);
    sigaction(SIGSEGV, &signal_handler_act, NULL);
#endif
#if HAVE_PRCTL && defined(PR_SET_UNALIGN)
    prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS);
#endif
#if ARCH_ARM && HAVE_ARMV5TE_EXTERNAL
    if (have_vfp(av_get_cpu_flags()) || have_neon(av_get_cpu_flags()))
        checkasm_checked_call = checkasm_checked_call_vfp;
#endif

    if (!tests[0].func || !cpus[0].flag) {
        fprintf(stderr, "checkasm: no tests to perform\n");
        return 0;
    }

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];
        unsigned long l;
        char *end;

        if (!strncmp(arg, "--bench", 7)) {
            if (bench_init() < 0)
                return 1;
            if (arg[7] == '=') {
                state.bench_pattern = arg + 8;
                state.bench_pattern_len = strlen(state.bench_pattern);
            } else
                state.bench_pattern = "*";
        } else if (!strncmp(arg, "--test=", 7)) {
            state.test_pattern = arg + 7;
        } else if (!strcmp(arg, "--csv")) {
            state.csv = 1; state.tsv = 0;
        } else if (!strcmp(arg, "--tsv")) {
            state.csv = 0; state.tsv = 1;
        } else if (!strcmp(arg, "--verbose") || !strcmp(arg, "-v")) {
            state.verbose = 1;
        } else if (!strncmp(arg, "--runs=", 7)) {
            l = strtoul(arg + 7, &end, 10);
            if (*end == '\0') {
                if (l > 30) {
                    fprintf(stderr, "checkasm: error: runs exponent must be within the range 0 <= 30\n");
                    usage(argv[0]);
                }
                bench_runs = 1U << l;
            } else {
                return usage(argv[0]);
            }
        } else if ((l = strtoul(arg, &end, 10)) <= UINT_MAX &&
                   *end == '\0') {
            seed = l;
        } else {
            return usage(argv[0]);
        }
    }

#if ARCH_AARCH64 && HAVE_SVE
    if (have_sve(av_get_cpu_flags()))
        snprintf(arch_info_buf, sizeof(arch_info_buf),
                 "SVE %d bits, ", 8 * ff_aarch64_sve_length());
#elif ARCH_RISCV && HAVE_RVV
    if (av_get_cpu_flags() & AV_CPU_FLAG_RVV_I32)
        snprintf(arch_info_buf, sizeof (arch_info_buf),
                 "%zu-bit vectors, ", 8 * ff_get_rv_vlenb());
#endif
    fprintf(stderr, "checkasm: %susing random seed %u\n", arch_info_buf, seed);
    av_lfg_init(&checkasm_lfg, seed);

    if (state.bench_pattern)
        fprintf(stderr, "checkasm: bench runs %" PRIu64 " (1 << %i)\n", bench_runs, av_log2(bench_runs));

    check_cpu_flag(NULL, 0);
    for (i = 0; cpus[i].flag; i++)
        check_cpu_flag(cpus[i].name, cpus[i].flag);

    if (state.num_failed) {
        fprintf(stderr, "checkasm: %d of %d tests have failed\n", state.num_failed, state.num_checked);
        ret = 1;
    } else {
        fprintf(stderr, "checkasm: all %d tests passed\n", state.num_checked);
        if (state.bench_pattern) {
            print_benchs(state.funcs);
        }
    }

    destroy_func_tree(state.funcs);
    bench_uninit();
    return ret;
}

/* Decide whether or not the specified function needs to be tested and
 * allocate/initialize data structures if needed. Returns a pointer to a
 * reference function if the function should be tested, otherwise NULL */
void *checkasm_check_func(void *func, const char *name, ...)
{
    char name_buf[256];
    void *ref = func;
    CheckasmFuncVersion *v;
    int name_length;
    va_list arg;

    va_start(arg, name);
    name_length = vsnprintf(name_buf, sizeof(name_buf), name, arg);
    va_end(arg);

    if (!func || name_length <= 0 || name_length >= sizeof(name_buf))
        return NULL;

    state.current_func = get_func(&state.funcs, name_buf);
    state.funcs->color = 1;
    v = &state.current_func->versions;

    if (v->func) {
        CheckasmFuncVersion *prev;
        do {
            /* Only test functions that haven't already been tested */
            if (v->func == func)
                return NULL;

            if (v->ok)
                ref = v->func;

            prev = v;
        } while ((v = v->next));

        v = prev->next = checkasm_malloc(sizeof(CheckasmFuncVersion));
    }

    v->func = func;
    v->ok = 1;
    v->cpu = state.cpu_flag;
    state.current_func_ver = v;

    if (state.cpu_flag)
        state.num_checked++;

    return ref;
}

/* Decide whether or not the current function needs to be benchmarked */
int checkasm_bench_func(void)
{
    return !state.num_failed && state.bench_pattern &&
           !wildstrcmp(state.current_func->name, state.bench_pattern);
}

/* Indicate that the current test has failed, return whether verbose printing
 * is requested. */
int checkasm_fail_func(const char *msg, ...)
{
    if (state.current_func_ver && state.current_func_ver->cpu &&
        state.current_func_ver->ok)
    {
        va_list arg;

        print_cpu_name();
        fprintf(stderr, "   %s_%s (", state.current_func->name, cpu_suffix(state.current_func_ver->cpu));
        va_start(arg, msg);
        vfprintf(stderr, msg, arg);
        va_end(arg);
        fprintf(stderr, ")\n");

        state.current_func_ver->ok = 0;
        state.num_failed++;
    }
    return state.verbose;
}

void checkasm_set_signal_handler_state(int enabled) {
    state.catch_signals = enabled;
}

int checkasm_handle_signal(int s) {
    if (s) {
#ifdef __GLIBC__
        checkasm_fail_func("fatal signal %d: %s", s, strsignal(s));
#else
        checkasm_fail_func(s == SIGFPE ? "fatal arithmetic error" :
                           s == SIGILL ? "illegal instruction" :
                           s == SIGBUS ? "bus error" :
                                         "segmentation fault");
#endif
    }
    return s;
}

/* Get the benchmark context of the current function */
CheckasmPerf *checkasm_get_perf_context(void)
{
    CheckasmPerf *perf = &state.current_func_ver->perf;
    memset(perf, 0, sizeof(*perf));
    perf->sysfd = state.sysfd;
    return perf;
}

/* Print the outcome of all tests performed since the last time this function was called */
void checkasm_report(const char *name, ...)
{
    static int prev_checked, prev_failed, max_length;

    if (state.num_checked > prev_checked) {
        int pad_length = max_length + 4;
        va_list arg;

        print_cpu_name();
        pad_length -= fprintf(stderr, " - %s.", state.current_test_name);
        va_start(arg, name);
        pad_length -= vfprintf(stderr, name, arg);
        va_end(arg);
        fprintf(stderr, "%*c", FFMAX(pad_length, 0) + 2, '[');

        if (state.num_failed == prev_failed)
            color_printf(COLOR_GREEN, "OK");
        else
            color_printf(COLOR_RED, "FAILED");
        fprintf(stderr, "]\n");

        prev_checked = state.num_checked;
        prev_failed  = state.num_failed;
    } else if (!state.cpu_flag) {
        /* Calculate the amount of padding required to make the output vertically aligned */
        int length = strlen(state.current_test_name);
        va_list arg;

        va_start(arg, name);
        length += vsnprintf(NULL, 0, name, arg);
        va_end(arg);

        if (length > max_length)
            max_length = length;
    }
}

static int check_err(const char *file, int line,
                     const char *name, int w, int h,
                     int *err)
{
    if (*err)
        return 0;
    if (!checkasm_fail_func("%s:%d", file, line))
        return 1;
    *err = 1;
    fprintf(stderr, "%s (%dx%d):\n", name, w, h);
    return 0;
}

#define DEF_CHECKASM_CHECK_FUNC(type, fmt) \
int checkasm_check_##type(const char *file, int line, \
                          const type *buf1, ptrdiff_t stride1, \
                          const type *buf2, ptrdiff_t stride2, \
                          int w, int h, const char *name, \
                          int align_w, int align_h, \
                          int padding) \
{ \
    int64_t aligned_w = (w - 1LL + align_w) & ~(align_w - 1); \
    int64_t aligned_h = (h - 1LL + align_h) & ~(align_h - 1); \
    int err = 0; \
    int y = 0; \
    av_assert0(aligned_w == (int32_t)aligned_w);\
    av_assert0(aligned_h == (int32_t)aligned_h);\
    stride1 /= sizeof(*buf1); \
    stride2 /= sizeof(*buf2); \
    for (y = 0; y < h; y++) \
        if (memcmp(&buf1[y*stride1], &buf2[y*stride2], w*sizeof(*buf1))) \
            break; \
    if (y != h) { \
        if (check_err(file, line, name, w, h, &err)) \
            return 1; \
        for (y = 0; y < h; y++) { \
            for (int x = 0; x < w; x++) \
                fprintf(stderr, " " fmt, buf1[x]); \
            fprintf(stderr, "    "); \
            for (int x = 0; x < w; x++) \
                fprintf(stderr, " " fmt, buf2[x]); \
            fprintf(stderr, "    "); \
            for (int x = 0; x < w; x++) \
                fprintf(stderr, "%c", buf1[x] != buf2[x] ? 'x' : '.'); \
            buf1 += stride1; \
            buf2 += stride2; \
            fprintf(stderr, "\n"); \
        } \
        buf1 -= h*stride1; \
        buf2 -= h*stride2; \
    } \
    for (y = -padding; y < 0; y++) \
        if (memcmp(&buf1[y*stride1 - padding], &buf2[y*stride2 - padding], \
                   (w + 2*padding)*sizeof(*buf1))) { \
            if (check_err(file, line, name, w, h, &err)) \
                return 1; \
            fprintf(stderr, " overwrite above\n"); \
            break; \
        } \
    for (y = aligned_h; y < aligned_h + padding; y++) \
        if (memcmp(&buf1[y*stride1 - padding], &buf2[y*stride2 - padding], \
                   (w + 2*padding)*sizeof(*buf1))) { \
            if (check_err(file, line, name, w, h, &err)) \
                return 1; \
            fprintf(stderr, " overwrite below\n"); \
            break; \
        } \
    for (y = 0; y < h; y++) \
        if (memcmp(&buf1[y*stride1 - padding], &buf2[y*stride2 - padding], \
                   padding*sizeof(*buf1))) { \
            if (check_err(file, line, name, w, h, &err)) \
                return 1; \
            fprintf(stderr, " overwrite left\n"); \
            break; \
        } \
    for (y = 0; y < h; y++) \
        if (memcmp(&buf1[y*stride1 + aligned_w], &buf2[y*stride2 + aligned_w], \
                   padding*sizeof(*buf1))) { \
            if (check_err(file, line, name, w, h, &err)) \
                return 1; \
            fprintf(stderr, " overwrite right\n"); \
            break; \
        } \
    return err; \
}

DEF_CHECKASM_CHECK_FUNC(uint8_t,  "%02x")
DEF_CHECKASM_CHECK_FUNC(uint16_t, "%04x")
DEF_CHECKASM_CHECK_FUNC(uint32_t, "%08x")
DEF_CHECKASM_CHECK_FUNC(int16_t,  "%6d")
DEF_CHECKASM_CHECK_FUNC(int32_t,  "%9d")
