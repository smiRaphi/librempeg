/*
 * TAK decoder
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

#include "libavutil/attributes.h"
#include "takdsp.h"
#include "config.h"

static void decorrelate_ls(const int32_t *p1, int32_t *p2, int length)
{
    int i;

    for (i = 0; i < length; i++) {
        uint32_t a = p1[i];
        uint32_t b = p2[i];
        p2[i]     = a + b;
    }
}

static void decorrelate_sr(int32_t *p1, const int32_t *p2, int length)
{
    int i;

    for (i = 0; i < length; i++) {
        uint32_t a = p1[i];
        uint32_t b = p2[i];
        p1[i]     = b - a;
    }
}

static void decorrelate_sm(int32_t *p1, int32_t *p2, int length)
{
    int i;

    for (i = 0; i < length; i++) {
        uint32_t a = p1[i];
        int32_t b = p2[i];
        a        -= b >> 1;
        p1[i]     = a;
        p2[i]     = a + b;
    }
}

static void decorrelate_sf(int32_t *p1, const int32_t *p2, int length, int dshift, int dfactor)
{
    int i;

    for (i = 0; i < length; i++) {
        uint32_t a = p1[i];
        int32_t b = p2[i];
        b         = (unsigned)((int)(dfactor * (unsigned)(b >> dshift) + 128) >> 8) << dshift;
        p1[i]     = b - a;
    }
}

av_cold void ff_takdsp_init(TAKDSPContext *c)
{
    c->decorrelate_ls = decorrelate_ls;
    c->decorrelate_sr = decorrelate_sr;
    c->decorrelate_sm = decorrelate_sm;
    c->decorrelate_sf = decorrelate_sf;

#if ARCH_RISCV
    ff_takdsp_init_riscv(c);
#elif ARCH_X86
    ff_takdsp_init_x86(c);
#endif
}
