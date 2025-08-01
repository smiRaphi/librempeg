/*
 * Copyright (c) 2017 Paul B Mahol
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

#include "libavutil/avassert.h"
#include "libavutil/colorspace.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "drawutils.h"
#include "filters.h"
#include "video.h"

enum { Y, U, V, A };
enum { R, G, B };

enum FillMode { FM_SMEAR, FM_MIRROR, FM_FIXED, FM_REFLECT, FM_WRAP, FM_FADE, FM_MARGINS, FM_NB_MODES };

typedef struct Borders {
    int left, right, top, bottom;
} Borders;

typedef struct FillBordersContext {
    const AVClass *class;
    int left, right, top, bottom;
    int mode;

    int nb_planes;
    int depth;
    Borders borders[4];
    int planewidth[4];
    int planeheight[4];
    uint8_t fill[4];
    uint8_t yuv_color[4];
    uint8_t rgba_color[4];

    void (*fillborders)(struct FillBordersContext *s, AVFrame *frame);
} FillBordersContext;

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_YUVA444P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12, AV_PIX_FMT_YUV422P12, AV_PIX_FMT_YUV444P12, AV_PIX_FMT_YUV440P12,
    AV_PIX_FMT_YUV420P14, AV_PIX_FMT_YUV422P14, AV_PIX_FMT_YUV444P14,
    AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_YUVA420P9, AV_PIX_FMT_YUVA422P9, AV_PIX_FMT_YUVA444P9,
    AV_PIX_FMT_YUVA420P10, AV_PIX_FMT_YUVA422P10, AV_PIX_FMT_YUVA444P10,
    AV_PIX_FMT_YUVA422P12, AV_PIX_FMT_YUVA444P12,
    AV_PIX_FMT_YUVA420P16, AV_PIX_FMT_YUVA422P16, AV_PIX_FMT_YUVA444P16,
    AV_PIX_FMT_GBRP, AV_PIX_FMT_GBRP9, AV_PIX_FMT_GBRP10,
    AV_PIX_FMT_GBRP12, AV_PIX_FMT_GBRP14, AV_PIX_FMT_GBRP16,
    AV_PIX_FMT_GBRAP, AV_PIX_FMT_GBRAP10, AV_PIX_FMT_GBRAP12, AV_PIX_FMT_GBRAP16,
    AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY9, AV_PIX_FMT_GRAY10, AV_PIX_FMT_GRAY12, AV_PIX_FMT_GRAY14, AV_PIX_FMT_GRAY16,
    AV_PIX_FMT_NONE
};

static void smear_borders8(FillBordersContext *s, AVFrame *frame)
{
    int p, y;

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *ptr = frame->data[p];
        ptrdiff_t linesize = frame->linesize[p];

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            memset(ptr + y * linesize,
                   *(ptr + y * linesize + s->borders[p].left),
                   s->borders[p].left);
            memset(ptr + y * linesize + s->planewidth[p] - s->borders[p].right,
                   *(ptr + y * linesize + s->planewidth[p] - s->borders[p].right - 1),
                   s->borders[p].right);
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + s->borders[p].top * linesize, s->planewidth[p]);
        }

        for (y = s->planeheight[p] - s->borders[p].bottom; y < s->planeheight[p]; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - 1) * linesize,
                   s->planewidth[p]);
        }
    }
}

static void smear_borders16(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint16_t *ptr = (uint16_t *)frame->data[p];
        ptrdiff_t linesize = frame->linesize[p] / 2;

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] =  *(ptr + y * linesize + s->borders[p].left);
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] =
                   *(ptr + y * linesize + s->planewidth[p] - s->borders[p].right - 1);
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + s->borders[p].top * linesize, s->planewidth[p] * 2);
        }

        for (y = s->planeheight[p] - s->borders[p].bottom; y < s->planeheight[p]; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - 1) * linesize,
                   s->planewidth[p] * 2);
        }
    }
}

static void mirror_borders8(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *ptr = frame->data[p];
        ptrdiff_t linesize = frame->linesize[p];

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] = ptr[y * linesize + s->borders[p].left * 2 - 1 - x];
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] =
                    ptr[y * linesize + s->planewidth[p] - s->borders[p].right - 1 - x];
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->borders[p].top * 2 - 1 - y) * linesize,
                   s->planewidth[p]);
        }

        for (y = 0; y < s->borders[p].bottom; y++) {
            memcpy(ptr + (s->planeheight[p] - s->borders[p].bottom + y) * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - 1 - y) * linesize,
                   s->planewidth[p]);
        }
    }
}

static void mirror_borders16(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint16_t *ptr = (uint16_t *)frame->data[p];
        ptrdiff_t linesize = frame->linesize[p] / 2;

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] = ptr[y * linesize + s->borders[p].left * 2 - 1 - x];
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] =
                    ptr[y * linesize + s->planewidth[p] - s->borders[p].right - 1 - x];
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->borders[p].top * 2 - 1 - y) * linesize,
                   s->planewidth[p] * 2);
        }

        for (y = 0; y < s->borders[p].bottom; y++) {
            memcpy(ptr + (s->planeheight[p] - s->borders[p].bottom + y) * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - 1 - y) * linesize,
                   s->planewidth[p] * 2);
        }
    }
}

static void fixed_borders8(FillBordersContext *s, AVFrame *frame)
{
    int p, y;

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *ptr = frame->data[p];
        uint8_t fill = s->fill[p];
        ptrdiff_t linesize = frame->linesize[p];

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            memset(ptr + y * linesize, fill, s->borders[p].left);
            memset(ptr + y * linesize + s->planewidth[p] - s->borders[p].right, fill,
                   s->borders[p].right);
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memset(ptr + y * linesize, fill, s->planewidth[p]);
        }

        for (y = s->planeheight[p] - s->borders[p].bottom; y < s->planeheight[p]; y++) {
            memset(ptr + y * linesize, fill, s->planewidth[p]);
        }
    }
}

static void fixed_borders16(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint16_t *ptr = (uint16_t *)frame->data[p];
        uint16_t fill = s->fill[p] << (s->depth - 8);
        ptrdiff_t linesize = frame->linesize[p] / 2;

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] = fill;
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] = fill;
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            for (x = 0; x < s->planewidth[p]; x++) {
                ptr[y * linesize + x] = fill;
            }
        }

        for (y = s->planeheight[p] - s->borders[p].bottom; y < s->planeheight[p]; y++) {
            for (x = 0; x < s->planewidth[p]; x++) {
                ptr[y * linesize + x] = fill;
            }
        }
    }
}

static void reflect_borders8(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *ptr = frame->data[p];
        ptrdiff_t linesize = frame->linesize[p];

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] = ptr[y * linesize + s->borders[p].left * 2 - x];
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] =
                    ptr[y * linesize + s->planewidth[p] - s->borders[p].right - 2 - x];
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->borders[p].top * 2 - y) * linesize,
                   s->planewidth[p]);
        }

        for (y = 0; y < s->borders[p].bottom; y++) {
            memcpy(ptr + (s->planeheight[p] - s->borders[p].bottom + y) * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - 2 - y) * linesize,
                   s->planewidth[p]);
        }
    }
}

static void reflect_borders16(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint16_t *ptr = (uint16_t *)frame->data[p];
        ptrdiff_t linesize = frame->linesize[p] / 2;

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] = ptr[y * linesize + s->borders[p].left * 2 - x];
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] =
                    ptr[y * linesize + s->planewidth[p] - s->borders[p].right - 2 - x];
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->borders[p].top * 2 - y) * linesize,
                   s->planewidth[p] * 2);
        }

        for (y = 0; y < s->borders[p].bottom; y++) {
            memcpy(ptr + (s->planeheight[p] - s->borders[p].bottom + y) * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - 2 - y) * linesize,
                   s->planewidth[p] * 2);
        }
    }
}

static void wrap_borders8(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *ptr = frame->data[p];
        ptrdiff_t linesize = frame->linesize[p];

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] = ptr[y * linesize + s->planewidth[p] - s->borders[p].right - s->borders[p].left + x];
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] =
                    ptr[y * linesize + s->borders[p].left + x];
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - s->borders[p].top + y) * linesize,
                   s->planewidth[p]);
        }

        for (y = 0; y < s->borders[p].bottom; y++) {
            memcpy(ptr + (s->planeheight[p] - s->borders[p].bottom + y) * linesize,
                   ptr + (s->borders[p].top + y) * linesize,
                   s->planewidth[p]);
        }
    }
}

static void wrap_borders16(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint16_t *ptr = (uint16_t *)frame->data[p];
        ptrdiff_t linesize = frame->linesize[p] / 2;

        for (y = s->borders[p].top; y < s->planeheight[p] - s->borders[p].bottom; y++) {
            for (x = 0; x < s->borders[p].left; x++) {
                ptr[y * linesize + x] = ptr[y * linesize + s->planewidth[p] - s->borders[p].right - s->borders[p].left + x];
            }

            for (x = 0; x < s->borders[p].right; x++) {
                ptr[y * linesize + s->planewidth[p] - s->borders[p].right + x] =
                    ptr[y * linesize + s->borders[p].left + x];
            }
        }

        for (y = 0; y < s->borders[p].top; y++) {
            memcpy(ptr + y * linesize,
                   ptr + (s->planeheight[p] - s->borders[p].bottom - s->borders[p].top + y) * linesize,
                   s->planewidth[p] * 2);
        }

        for (y = 0; y < s->borders[p].bottom; y++) {
            memcpy(ptr + (s->planeheight[p] - s->borders[p].bottom + y) * linesize,
                   ptr + (s->borders[p].top + y) * linesize,
                   s->planewidth[p] * 2);
        }
    }
}

static int lerp8(int fill, int src, int pos, int size)
{
    return av_clip_uint8(((fill * 256 * pos / size) + (src * 256 * (size - pos) / size)) >> 8);
}

static int lerp16(int fill, int src, int pos, int size, int depth)
{
    return av_clip_uintp2_c(((fill * (1LL << depth) * pos / size) + (src * (1LL << depth) * (size - pos) / size)) >> depth, depth);
}

static void fade_borders8(FillBordersContext *s, AVFrame *frame)
{
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint8_t *ptr = frame->data[p];
        const uint8_t fill = s->fill[p];
        const ptrdiff_t linesize = frame->linesize[p];
        const int start_left = s->borders[p].left;
        const int start_right = s->planewidth[p] - s->borders[p].right;
        const int start_top = s->borders[p].top;
        const int start_bottom = s->planeheight[p] - s->borders[p].bottom;

        for (y = 0; y < start_top; y++) {
            for (x = 0; x < s->planewidth[p]; x++) {
                int src = ptr[y * linesize + x];
                ptr[y * linesize + x] = lerp8(fill, src, start_top - y, start_top);
            }
        }

        for (y = start_bottom; y < s->planeheight[p]; y++) {
            for (x = 0; x < s->planewidth[p]; x++) {
                int src = ptr[y * linesize + x];
                ptr[y * linesize + x] = lerp8(fill, src, y - start_bottom, s->borders[p].bottom);
            }
        }

        for (y = 0; y < s->planeheight[p]; y++) {
            for (x = 0; x < start_left; x++) {
                int src = ptr[y * linesize + x];
                ptr[y * linesize + x] = lerp8(fill, src, start_left - x, start_left);
            }

            for (x = 0; x < s->borders[p].right; x++) {
                int src = ptr[y * linesize + start_right + x];
                ptr[y * linesize + start_right + x] = lerp8(fill, src, x, s->borders[p].right);
            }
        }
    }
}

static void fade_borders16(FillBordersContext *s, AVFrame *frame)
{
    const int depth = s->depth;
    int p, y, x;

    for (p = 0; p < s->nb_planes; p++) {
        uint16_t *ptr = (uint16_t *)frame->data[p];
        const uint16_t fill = s->fill[p] << (depth - 8);
        const ptrdiff_t linesize = frame->linesize[p] / 2;
        const int start_left = s->borders[p].left;
        const int start_right = s->planewidth[p] - s->borders[p].right;
        const int start_top = s->borders[p].top;
        const int start_bottom = s->planeheight[p] - s->borders[p].bottom;

        for (y = 0; y < start_top; y++) {
            for (x = 0; x < s->planewidth[p]; x++) {
                int src = ptr[y * linesize + x];
                ptr[y * linesize + x] = lerp16(fill, src, start_top - y, start_top, depth);
            }
        }

        for (y = start_bottom; y < s->planeheight[p]; y++) {
            for (x = 0; x < s->planewidth[p]; x++) {
                int src = ptr[y * linesize + x];
                ptr[y * linesize + x] = lerp16(fill, src, y - start_bottom, s->borders[p].bottom, depth);
            }
        }

        for (y = 0; y < s->planeheight[p]; y++) {
            for (x = 0; x < start_left; x++) {
                int src = ptr[y * linesize + x];
                ptr[y * linesize + x] = lerp16(fill, src, start_left - x, start_left, depth);
            }

            for (x = 0; x < s->borders[p].right; x++) {
                int src = ptr[y * linesize + start_right + x];
                ptr[y * linesize + start_right + x] = lerp16(fill, src, x, s->borders[p].right, depth);
            }
        }
    }
}

static void margins_borders8(FillBordersContext *s, AVFrame *frame)
{
    for (int p = 0; p < s->nb_planes; p++) {
        uint8_t *ptr = (uint8_t *)frame->data[p];
        const ptrdiff_t linesize = frame->linesize[p];
        const int left = s->borders[p].left;
        const int right = s->borders[p].right;
        const int top = s->borders[p].top;
        const int bottom = s->borders[p].bottom;
        const int width = s->planewidth[p];
        const int height = s->planeheight[p];

        for (int y = top; y < height - bottom; y++) {
            memset(ptr + linesize * y, ptr[linesize * y + left], left);
            memset(ptr + linesize * y + width - right, (ptr + linesize * y + width - right)[-1], right);
        }

        for (int y = top - 1; y >= 0; y--) {
            ptr[linesize * y] = ptr[linesize * (y + 1)];
            memcpy(ptr + linesize * y + width - 8, ptr + linesize * (y + 1) + width - 8, 8);

            for (int x = 1; x < width - 8; x++) {
                int prev = ptr[linesize * (y + 1) + x - 1];
                int cur  = ptr[linesize * (y + 1) + x];
                int next = ptr[linesize * (y + 1) + x + 1];

                ptr[linesize * y + x] = (3 * prev + 2 * cur + 3 * next + 4) >> 3;
            }
        }

        for (int y = height - bottom; y < height; y++) {
            ptr[linesize * y] = ptr[linesize * (y - 1)];
            memcpy(ptr + linesize * y + width - 8, ptr + linesize * (y - 1) + width - 8, 8);

            for (int x = 1; x < width - 8; x++) {
                int prev = ptr[linesize * (y - 1) + x - 1];
                int cur  = ptr[linesize * (y - 1) + x];
                int next = ptr[linesize * (y - 1) + x + 1];

                ptr[linesize * y + x] = (3 * prev + 2 * cur + 3 * next + 4) >> 3;
            }
        }
    }
}

static void margins_borders16(FillBordersContext *s, AVFrame *frame)
{
    for (int p = 0; p < s->nb_planes; p++) {
        uint16_t *ptr = (uint16_t *)frame->data[p];
        const ptrdiff_t linesize = frame->linesize[p] / 2;
        const int left = s->borders[p].left;
        const int right = s->borders[p].right;
        const int top = s->borders[p].top;
        const int bottom = s->borders[p].bottom;
        const int width = s->planewidth[p];
        const int height = s->planeheight[p];

        for (int y = top; y < height - bottom; y++) {
            for (int x = 0; x < left; x++)
                ptr[linesize * y + x] = ptr[linesize * y + left];

            for (int x = 0; x < right; x++)
                ptr[linesize * y + width - right + x] = ptr[linesize * y + width - right - 1];
        }

        for (int y = top - 1; y >= 0; y--) {
            ptr[linesize * y] = ptr[linesize * (y + 1)];
            memcpy(ptr + linesize * y + width - 8, ptr + linesize * (y + 1) + width - 8, 16);

            for (int x = 1; x < width - 8; x++) {
                int prev = ptr[linesize * (y + 1) + x - 1];
                int cur  = ptr[linesize * (y + 1) + x];
                int next = ptr[linesize * (y + 1) + x + 1];

                ptr[linesize * y + x] = (3 * prev + 2 * cur + 3 * next + 4) >> 3;
            }
        }

        for (int y = height - bottom; y < height; y++) {
            ptr[linesize * y] = ptr[linesize * (y - 1)];
            memcpy(ptr + linesize * y + width - 8, ptr + linesize * (y - 1) + width - 8, 16);

            for (int x = 1; x < width - 8; x++) {
                int prev = ptr[linesize * (y - 1) + x - 1];
                int cur  = ptr[linesize * (y - 1) + x];
                int next = ptr[linesize * (y - 1) + x + 1];

                ptr[linesize * y + x] = (3 * prev + 2 * cur + 3 * next + 4) >> 3;
            }
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    FillBordersContext *s = inlink->dst->priv;

    s->fillborders(s, frame);

    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    FillBordersContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    s->nb_planes = desc->nb_components;
    s->depth = desc->comp[0].depth;

    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;
    s->planewidth[1]  = s->planewidth[2]  = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0]  = s->planewidth[3]  = inlink->w;

    if (inlink->w < s->left + s->right ||
        inlink->w <= s->left ||
        inlink->w <= s->right ||
        inlink->h < s->top + s->bottom ||
        inlink->h <= s->top ||
        inlink->h <= s->bottom ||
        inlink->w < s->left * 2 ||
        inlink->w < s->right * 2 ||
        inlink->h < s->top * 2 ||
        inlink->h < s->bottom * 2) {
        av_log(ctx, AV_LOG_ERROR, "Borders are bigger than input frame size.\n");
        return AVERROR(EINVAL);
    }

    s->borders[0].left   = s->borders[3].left = s->left;
    s->borders[0].right  = s->borders[3].right = s->right;
    s->borders[0].top    = s->borders[3].top = s->top;
    s->borders[0].bottom = s->borders[3].bottom = s->bottom;

    s->borders[1].left   = s->left >> desc->log2_chroma_w;
    s->borders[1].right  = s->right >> desc->log2_chroma_w;
    s->borders[1].top    = s->top >> desc->log2_chroma_h;
    s->borders[1].bottom = s->bottom >> desc->log2_chroma_h;

    s->borders[2].left   = s->left >> desc->log2_chroma_w;
    s->borders[2].right  = s->right >> desc->log2_chroma_w;
    s->borders[2].top    = s->top >> desc->log2_chroma_h;
    s->borders[2].bottom = s->bottom >> desc->log2_chroma_h;

    switch (s->mode) {
    case FM_SMEAR:  s->fillborders = s->depth <= 8 ? smear_borders8  : smear_borders16;  break;
    case FM_MIRROR: s->fillborders = s->depth <= 8 ? mirror_borders8 : mirror_borders16; break;
    case FM_FIXED:  s->fillborders = s->depth <= 8 ? fixed_borders8  : fixed_borders16;  break;
    case FM_REFLECT:s->fillborders = s->depth <= 8 ? reflect_borders8: reflect_borders16;break;
    case FM_WRAP:   s->fillborders = s->depth <= 8 ? wrap_borders8   : wrap_borders16;   break;
    case FM_FADE:   s->fillborders = s->depth <= 8 ? fade_borders8   : fade_borders16;   break;
    case FM_MARGINS:s->fillborders = s->depth <= 8 ? margins_borders8: margins_borders16;break;
    default: av_assert0(0);
    }

    s->yuv_color[Y] = RGB_TO_Y_CCIR(s->rgba_color[R], s->rgba_color[G], s->rgba_color[B]);
    s->yuv_color[U] = RGB_TO_U_CCIR(s->rgba_color[R], s->rgba_color[G], s->rgba_color[B], 0);
    s->yuv_color[V] = RGB_TO_V_CCIR(s->rgba_color[R], s->rgba_color[G], s->rgba_color[B], 0);
    s->yuv_color[A] = s->rgba_color[A];

    if (desc->flags & AV_PIX_FMT_FLAG_RGB) {
        uint8_t rgba_map[4];
        int i;

        ff_fill_rgba_map(rgba_map, inlink->format);
        for (i = 0; i < 4; i++)
            s->fill[rgba_map[i]] = s->rgba_color[i];
    } else {
        memcpy(s->fill, s->yuv_color, sizeof(s->yuv_color));
    }

    return 0;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *arg)
{
    int ret = ff_filter_process_command(ctx, cmd, arg);

    if (ret < 0)
        return ret;

    return config_input(ctx->inputs[0]);
}

#define OFFSET(x) offsetof(FillBordersContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption fillborders_options[] = {
    { "left",   "set the left fill border",   OFFSET(left),   AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX,    FLAGS },
    { "right",  "set the right fill border",  OFFSET(right),  AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX,    FLAGS },
    { "top",    "set the top fill border",    OFFSET(top),    AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX,    FLAGS },
    { "bottom", "set the bottom fill border", OFFSET(bottom), AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX,    FLAGS },
    { "mode",   "set the fill borders mode",  OFFSET(mode),   AV_OPT_TYPE_INT, {.i64=FM_SMEAR}, 0, FM_NB_MODES-1, FLAGS, .unit = "mode" },
        { "smear",  NULL, 0, AV_OPT_TYPE_CONST, {.i64=FM_SMEAR},  0, 0, FLAGS, .unit = "mode" },
        { "mirror", NULL, 0, AV_OPT_TYPE_CONST, {.i64=FM_MIRROR}, 0, 0, FLAGS, .unit = "mode" },
        { "fixed",  NULL, 0, AV_OPT_TYPE_CONST, {.i64=FM_FIXED},  0, 0, FLAGS, .unit = "mode" },
        { "reflect",NULL, 0, AV_OPT_TYPE_CONST, {.i64=FM_REFLECT},0, 0, FLAGS, .unit = "mode" },
        { "wrap",   NULL, 0, AV_OPT_TYPE_CONST, {.i64=FM_WRAP},   0, 0, FLAGS, .unit = "mode" },
        { "fade",   NULL, 0, AV_OPT_TYPE_CONST, {.i64=FM_FADE},   0, 0, FLAGS, .unit = "mode" },
        { "margins",NULL, 0, AV_OPT_TYPE_CONST, {.i64=FM_MARGINS},0, 0, FLAGS, .unit = "mode" },
    { "color",  "set the color for the fixed/fade mode", OFFSET(rgba_color), AV_OPT_TYPE_COLOR, {.str = "black"}, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(fillborders);

static const AVFilterPad fillborders_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .flags          = AVFILTERPAD_FLAG_NEEDS_WRITABLE,
        .config_props   = config_input,
        .filter_frame   = filter_frame,
    },
};

const FFFilter ff_vf_fillborders = {
    .p.name        = "fillborders",
    .p.description = NULL_IF_CONFIG_SMALL("Fill borders of the input video."),
    .p.priv_class  = &fillborders_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .priv_size     = sizeof(FillBordersContext),
    FILTER_INPUTS(fillborders_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
    .process_command = process_command,
};
