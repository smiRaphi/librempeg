/*
 * Copyright (c) 2012 Mans Rullgard <mans@mansr.com>
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

#include "libavutil/attributes.h"
#include "libavutil/internal.h"
#include "libavutil/samplefmt.h"
#include "flacdsp.h"
#include "config.h"

#define SAMPLE_SIZE 16
#define PLANAR 0
#include "flacdsp_template.c"

#undef  PLANAR
#define PLANAR 1
#include "flacdsp_template.c"

#undef  SAMPLE_SIZE
#undef  PLANAR
#define SAMPLE_SIZE 32
#define PLANAR 0
#include "flacdsp_template.c"

#undef  PLANAR
#define PLANAR 1
#include "flacdsp_template.c"

static void flac_lpc_16_c(int32_t *decoded, const int coeffs[32],
                          int pred_order, int qlevel, int len)
{
    int i, j;

    for (i = pred_order; i < len - 1; i += 2, decoded += 2) {
        SUINT c = coeffs[0];
        SUINT d = decoded[0];
        int s0 = 0, s1 = 0;
        for (j = 1; j < pred_order; j++) {
            s0 += c*d;
            d = decoded[j];
            s1 += c*d;
            c = coeffs[j];
        }
        s0 += c*d;
        d = decoded[j] += (SUINT)(s0 >> qlevel);
        s1 += c*d;
        decoded[j + 1] += (SUINT)(s1 >> qlevel);
    }
    if (i < len) {
        int sum = 0;
        for (j = 0; j < pred_order; j++)
            sum += coeffs[j] * (SUINT)decoded[j];
        decoded[j] = decoded[j] + (unsigned)(sum >> qlevel);
    }
}

static void flac_lpc_32_c(int32_t *decoded, const int coeffs[32],
                          int pred_order, int qlevel, int len)
{
    int i, j;

    for (i = pred_order; i < len; i++, decoded++) {
        int64_t sum = 0;
        for (j = 0; j < pred_order; j++)
            sum += (int64_t)coeffs[j] * decoded[j];
        decoded[j] += sum >> qlevel;
    }

}

static void flac_lpc_33_c(int64_t *decoded, const int32_t *residual,
                          const int coeffs[32], int pred_order,
                          int qlevel, int len)
{
    int i, j;

    for (i = pred_order; i < len; i++, decoded++) {
        int64_t sum = 0;
        for (j = 0; j < pred_order; j++)
            sum += (int64_t)coeffs[j] * (uint64_t)decoded[j];
        decoded[j] = (uint64_t)residual[i] + (uint64_t)(sum >> qlevel);
    }
}

static void flac_wasted_32_c(int32_t *decoded, int wasted, int len)
{
    for (int i = 0; i < len; i++)
        decoded[i] = (unsigned)decoded[i] << wasted;
}

static void flac_wasted_33_c(int64_t *decoded, const int32_t *residual,
                             int wasted, int len)
{
    for (int i = 0; i < len; i++)
        decoded[i] = (uint64_t)residual[i] << wasted;
}

av_cold void ff_flacdsp_init(FLACDSPContext *c, enum AVSampleFormat fmt, int channels)
{
    c->lpc16        = flac_lpc_16_c;
    c->lpc32        = flac_lpc_32_c;
    c->lpc33        = flac_lpc_33_c;

    c->wasted32     = flac_wasted_32_c;
    c->wasted33     = flac_wasted_33_c;

    switch (fmt) {
    case AV_SAMPLE_FMT_S32:
        c->decorrelate[0] = flac_decorrelate_indep_c_32;
        c->decorrelate[1] = flac_decorrelate_ls_c_32;
        c->decorrelate[2] = flac_decorrelate_rs_c_32;
        c->decorrelate[3] = flac_decorrelate_ms_c_32;
        break;

    case AV_SAMPLE_FMT_S32P:
        c->decorrelate[0] = flac_decorrelate_indep_c_32p;
        c->decorrelate[1] = flac_decorrelate_ls_c_32p;
        c->decorrelate[2] = flac_decorrelate_rs_c_32p;
        c->decorrelate[3] = flac_decorrelate_ms_c_32p;
        break;

    case AV_SAMPLE_FMT_S16:
        c->decorrelate[0] = flac_decorrelate_indep_c_16;
        c->decorrelate[1] = flac_decorrelate_ls_c_16;
        c->decorrelate[2] = flac_decorrelate_rs_c_16;
        c->decorrelate[3] = flac_decorrelate_ms_c_16;
        break;

    case AV_SAMPLE_FMT_S16P:
        c->decorrelate[0] = flac_decorrelate_indep_c_16p;
        c->decorrelate[1] = flac_decorrelate_ls_c_16p;
        c->decorrelate[2] = flac_decorrelate_rs_c_16p;
        c->decorrelate[3] = flac_decorrelate_ms_c_16p;
        break;
    }

#if ARCH_ARM
    ff_flacdsp_init_arm(c, fmt, channels);
#elif ARCH_RISCV
    ff_flacdsp_init_riscv(c, fmt, channels);
#elif ARCH_X86
    ff_flacdsp_init_x86(c, fmt, channels);
#endif
}
