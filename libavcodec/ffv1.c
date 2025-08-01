/*
 * FFV1 codec for libavcodec
 *
 * Copyright (c) 2003-2013 Michael Niedermayer <michaelni@gmx.at>
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
 * FF Video Codec 1 (a lossless codec)
 */

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "ffv1.h"
#include "libavutil/refstruct.h"

av_cold int ff_ffv1_common_init(AVCodecContext *avctx, FFV1Context *s)
{
    if (!avctx->width || !avctx->height)
        return AVERROR_INVALIDDATA;

    s->avctx = avctx;
    s->flags = avctx->flags;

    s->width  = avctx->width;
    s->height = avctx->height;

    // defaults
    s->num_h_slices = 1;
    s->num_v_slices = 1;

    return 0;
}

static void planes_free(AVRefStructOpaque opaque, void *obj)
{
    PlaneContext *planes = obj;

    for (int i = 0; i < MAX_PLANES; i++) {
        PlaneContext *p = &planes[i];

        av_freep(&p->state);
        av_freep(&p->vlc_state);
    }
}

PlaneContext* ff_ffv1_planes_alloc(void)
{
    return av_refstruct_alloc_ext(sizeof(PlaneContext) * MAX_PLANES,
                                  0, NULL, planes_free);
}

av_cold int ff_ffv1_init_slice_state(const FFV1Context *f,
                                     FFV1SliceContext *sc)
{
    int j, i;

    for (j = 0; j < f->plane_count; j++) {
        PlaneContext *const p = &sc->plane[j];

        if (f->ac != AC_GOLOMB_RICE) {
            if (!p->state)
                p->state = av_malloc_array(p->context_count, CONTEXT_SIZE *
                                     sizeof(uint8_t));
            if (!p->state)
                return AVERROR(ENOMEM);
        } else {
            if (!p->vlc_state) {
                p->vlc_state = av_calloc(p->context_count, sizeof(*p->vlc_state));
                if (!p->vlc_state)
                    return AVERROR(ENOMEM);
                for (i = 0; i < p->context_count; i++) {
                    p->vlc_state[i].error_sum = 4;
                    p->vlc_state[i].count     = 1;
                }
            }
        }
    }

    if (f->ac == AC_RANGE_CUSTOM_TAB) {
        //FIXME only redo if state_transition changed
        for (j = 1; j < 256; j++) {
            sc->c. one_state[      j] = f->state_transition[j];
            sc->c.zero_state[256 - j] = 256 - sc->c.one_state[j];
        }
    }

    return 0;
}

av_cold int ff_ffv1_init_slices_state(FFV1Context *f)
{
    int i, ret;
    for (i = 0; i < f->max_slice_count; i++) {
        if ((ret = ff_ffv1_init_slice_state(f, &f->slices[i])) < 0)
            return AVERROR(ENOMEM);
    }
    return 0;
}

int ff_need_new_slices(int width, int num_h_slices, int chroma_shift) {
    int mpw = 1<<chroma_shift;
    int i = width * (int64_t)(num_h_slices - 1) / num_h_slices;

    return width % mpw && (width - i) % mpw == 0;
}

int ff_slice_coord(const FFV1Context *f, int width, int sx, int num_h_slices, int chroma_shift) {
    int mpw = 1<<chroma_shift;
    int awidth = FFALIGN(width, mpw);

    if (f->combined_version <= 0x40002)
        return width * sx / num_h_slices;

    sx = (2LL * awidth * sx + num_h_slices * mpw) / (2 * num_h_slices * mpw) * mpw;
    if (sx == awidth)
        sx = width;
    return sx;
}

av_cold int ff_ffv1_init_slice_contexts(FFV1Context *f)
{
    int max_slice_count = f->num_h_slices * f->num_v_slices;

    av_assert0(max_slice_count > 0);

    f->slices = av_calloc(max_slice_count, sizeof(*f->slices));
    if (!f->slices)
        return AVERROR(ENOMEM);

    f->max_slice_count = max_slice_count;

    for (int i = 0; i < max_slice_count; i++) {
        FFV1SliceContext *sc = &f->slices[i];
        int sx          = i % f->num_h_slices;
        int sy          = i / f->num_h_slices;
        int sxs         = ff_slice_coord(f, f->avctx->width , sx    , f->num_h_slices, f->chroma_h_shift);
        int sxe         = ff_slice_coord(f, f->avctx->width , sx + 1, f->num_h_slices, f->chroma_h_shift);
        int sys         = ff_slice_coord(f, f->avctx->height, sy   ,  f->num_v_slices, f->chroma_v_shift);
        int sye         = ff_slice_coord(f, f->avctx->height, sy + 1, f->num_v_slices, f->chroma_v_shift);

        sc->slice_width  = sxe - sxs;
        sc->slice_height = sye - sys;
        sc->slice_x      = sxs;
        sc->slice_y      = sys;
        sc->sx           = sx;
        sc->sy           = sy;

        sc->sample_buffer = av_malloc_array((f->width + 6), 3 * MAX_PLANES *
                                            sizeof(*sc->sample_buffer));
        sc->sample_buffer32 = av_malloc_array((f->width + 6), 3 * MAX_PLANES *
                                              sizeof(*sc->sample_buffer32));
        if (!sc->sample_buffer || !sc->sample_buffer32)
            return AVERROR(ENOMEM);

        sc->plane = ff_ffv1_planes_alloc();
        if (!sc->plane)
            return AVERROR(ENOMEM);
    }

    return 0;
}

int ff_ffv1_allocate_initial_states(FFV1Context *f)
{
    int i;

    for (i = 0; i < f->quant_table_count; i++) {
        f->initial_states[i] = av_malloc_array(f->context_count[i],
                                         sizeof(*f->initial_states[i]));
        if (!f->initial_states[i])
            return AVERROR(ENOMEM);
        memset(f->initial_states[i], 128,
               f->context_count[i] * sizeof(*f->initial_states[i]));
    }
    return 0;
}

void ff_ffv1_clear_slice_state(const FFV1Context *f, FFV1SliceContext *sc)
{
    int i, j;

    for (i = 0; i < f->plane_count; i++) {
        PlaneContext *p = &sc->plane[i];

        if (f->ac != AC_GOLOMB_RICE) {
            if (f->initial_states[p->quant_table_index]) {
                memcpy(p->state, f->initial_states[p->quant_table_index],
                       CONTEXT_SIZE * p->context_count);
            } else
                memset(p->state, 128, CONTEXT_SIZE * p->context_count);
        } else {
            for (j = 0; j < p->context_count; j++) {
                p->vlc_state[j].drift     = 0;
                p->vlc_state[j].error_sum = 4;    //FFMAX((RANGE + 32)/64, 2);
                p->vlc_state[j].bias      = 0;
                p->vlc_state[j].count     = 1;
            }
        }
    }
}

void ff_ffv1_compute_bits_per_plane(const FFV1Context *f, FFV1SliceContext *sc, int bits[4], int *offset, int mask[4], int bits_per_raw_sample)
{
    // to simplify we use the remap_count as the symbol range in each plane
    if (!sc->remap) {
        sc->remap_count[0] =
        sc->remap_count[1] =
        sc->remap_count[2] =
        sc->remap_count[3] = 1 << (bits_per_raw_sample > 0 ? bits_per_raw_sample : 8);
    }

    if (sc->remap)
        av_assert0(bits_per_raw_sample > 8); //breaks with lbd, needs review if added

    //bits with no RCT
    for (int p=0; p<3+f->transparency; p++) {
        bits[p] = av_ceil_log2(sc->remap_count[p]);
        if (mask)
            mask[p] = (1<<bits[p]) - 1;
    }

    //RCT
    if (sc->slice_coding_mode == 0) {
        *offset = sc->remap_count[0];

        bits[0] = av_ceil_log2(FFMAX3(sc->remap_count[0], sc->remap_count[1], sc->remap_count[2]));
        bits[1] = av_ceil_log2(sc->remap_count[0] + sc->remap_count[1]);
        bits[2] = av_ceil_log2(sc->remap_count[0] + sc->remap_count[2]);

        //old version coded a bit more than needed
        if (f->combined_version < 0x40008) {
            bits[0]++;
            if(f->transparency)
                bits[3]++;
        }
    }
}

int ff_ffv1_get_symbol(RangeCoder *c, uint8_t *state, int is_signed)
{
    return get_symbol_inline(c, state, is_signed);
}

av_cold void ff_ffv1_close(FFV1Context *s)
{
    int i, j;

    for (j = 0; j < s->max_slice_count; j++) {
        FFV1SliceContext *sc = &s->slices[j];

        av_freep(&sc->sample_buffer);
        av_freep(&sc->sample_buffer32);
        for(int p = 0; p < 4 ; p++) {
            av_freep(&sc->fltmap[p]);
            av_freep(&sc->fltmap32[p]);
            sc->fltmap_size  [p] = 0;
            sc->fltmap32_size[p] = 0;
        }

        av_refstruct_unref(&sc->plane);
    }

    av_refstruct_unref(&s->slice_damaged);

    for (j = 0; j < s->quant_table_count; j++) {
        av_freep(&s->initial_states[j]);
        for (i = 0; i < s->max_slice_count; i++) {
            FFV1SliceContext *sc = &s->slices[i];
            av_freep(&sc->rc_stat2[j]);
        }
        av_freep(&s->rc_stat2[j]);
    }

    av_freep(&s->slices);
}
