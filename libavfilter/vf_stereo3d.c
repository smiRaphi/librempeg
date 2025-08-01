/*
 * Copyright (c) 2010 Gordon Schmidt <gordon.schmidt <at> s2000.tu-chemnitz.de>
 * Copyright (c) 2013-2015 Paul B Mahol
 *
 * This file is part of Librempeg.
 *
 * Librempeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Librempeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "drawutils.h"
#include "filters.h"
#include "formats.h"
#include "video.h"
#include "stereo3d.h"

enum StereoCode {
    ANAGLYPH_RC_GRAY,   // anaglyph red/cyan gray
    ANAGLYPH_RC_HALF,   // anaglyph red/cyan half colored
    ANAGLYPH_RC_COLOR,  // anaglyph red/cyan colored
    ANAGLYPH_RC_DUBOIS, // anaglyph red/cyan dubois
    ANAGLYPH_GM_GRAY,   // anaglyph green/magenta gray
    ANAGLYPH_GM_HALF,   // anaglyph green/magenta half colored
    ANAGLYPH_GM_COLOR,  // anaglyph green/magenta colored
    ANAGLYPH_GM_DUBOIS, // anaglyph green/magenta dubois
    ANAGLYPH_YB_GRAY,   // anaglyph yellow/blue gray
    ANAGLYPH_YB_HALF,   // anaglyph yellow/blue half colored
    ANAGLYPH_YB_COLOR,  // anaglyph yellow/blue colored
    ANAGLYPH_YB_DUBOIS, // anaglyph yellow/blue dubois
    ANAGLYPH_RB_GRAY,   // anaglyph red/blue gray
    ANAGLYPH_RG_GRAY,   // anaglyph red/green gray
    MONO,               // mono output for debugging
    INTERLEAVE_ROWS,    // row-interleave
    SIDE_BY_SIDE,       // side by side parallel
    SIDE_BY_SIDE_2,     // side by side parallel with half width resolution
    ABOVE_BELOW,        // above-below
    ABOVE_BELOW_2,      // above-below with half height resolution
    ALTERNATING,        // alternating frames
    CHECKERBOARD,       // checkerboard pattern
    INTERLEAVE_COLS,    // column-interleave
    HDMI,               // HDMI frame pack
    STEREO_CODE_COUNT   // TODO: needs autodetection
};

enum EyesCode {
    LEFT,               // left first, right second
    RIGHT,              // right first, left second
    EYES_CODE_COUNT
};

typedef struct StereoComponent {
    int format;                 ///< StereoCode
    int eyes;                   ///< EyesCode
    int width, height;
    int off_left, off_right;
    int off_lstep, off_rstep;
    int row_left, row_right;
    int row_step;
} StereoComponent;

static const int ana_coeff[][3][6] = {
  [ANAGLYPH_RB_GRAY]   =
    {{19595, 38470,  7471,     0,     0,     0},
     {    0,     0,     0,     0,     0,     0},
     {    0,     0,     0, 19595, 38470,  7471}},
  [ANAGLYPH_RG_GRAY]   =
    {{19595, 38470,  7471,     0,     0,     0},
     {    0,     0,     0, 19595, 38470,  7471},
     {    0,     0,     0,     0,     0,     0}},
  [ANAGLYPH_RC_GRAY]   =
    {{19595, 38470,  7471,     0,     0,     0},
     {    0,     0,     0, 19595, 38470,  7471},
     {    0,     0,     0, 19595, 38470,  7471}},
  [ANAGLYPH_GM_GRAY]   =
    {{    0,     0,     0, 19595, 38470,  7471},
     {19595, 38470,  7471,     0,     0,     0},
     {    0,     0,     0, 19595, 38470,  7471}},
  [ANAGLYPH_YB_GRAY]   =
    {{    0,     0,     0, 19595, 38470,  7471},
     {    0,     0,     0, 19595, 38470,  7471},
     {19595, 38470,  7471,     0,     0,     0}},
  [ANAGLYPH_RC_HALF]   =
    {{19595, 38470,  7471,     0,     0,     0},
     {    0,     0,     0,     0, 65536,     0},
     {    0,     0,     0,     0,     0, 65536}},
  [ANAGLYPH_GM_HALF]   =
    {{    0,     0,     0, 65536,     0,     0},
     {19595, 38470,  7471,     0,     0,     0},
     {    0,     0,     0,     0,     0, 65536}},
  [ANAGLYPH_YB_HALF]   =
    {{    0,     0,     0, 65536,     0,     0},
     {    0,     0,     0,     0, 65536,     0},
     {19595, 38470,  7471,     0,     0,     0}},
  [ANAGLYPH_RC_COLOR]  =
    {{65536,     0,     0,     0,     0,     0},
     {    0,     0,     0,     0, 65536,     0},
     {    0,     0,     0,     0,     0, 65536}},
  [ANAGLYPH_GM_COLOR]  =
    {{    0,     0,     0, 65536,     0,     0},
     {    0, 65536,     0,     0,     0,     0},
     {    0,     0,     0,     0,     0, 65536}},
  [ANAGLYPH_YB_COLOR]  =
    {{    0,     0,     0, 65536,     0,     0},
     {    0,     0,     0,     0, 65536,     0},
     {    0,     0, 65536,     0,     0,     0}},
  [ANAGLYPH_RC_DUBOIS] =
    {{29884, 32768, 11534, -2818, -5767,  -131},
     {-2621, -2490, -1049, 24773, 48103, -1180},
     { -983, -1376,  -328, -4719, -7406, 80347}},
  [ANAGLYPH_GM_DUBOIS]  =
    {{-4063,-10354, -2556, 34669, 46203,  1573},
     {18612, 43778,  9372, -1049,  -983, -4260},
     { -983, -1769,  1376,   590,  4915, 61407}},
  [ANAGLYPH_YB_DUBOIS] =
    {{69599,-13435,19595,  -1048, -8061, -1114},
     {-1704, 59507, 4456,    393,  4063, -1114},
     {-2490,-11338, 1442,   6160, 12124, 59703}},
};

typedef struct Stereo3DContext {
    const AVClass *class;
    StereoComponent in, out;
    int width, height;
    const int *ana_matrix[3];
    int nb_planes;
    int linesize[4];
    int pheight[4];
    int hsub, vsub;
    int pixstep[4];
    AVFrame *prev;
    int blanks;
    int in_off_left[4], in_off_right[4];
    AVRational aspect;
    Stereo3DDSPContext dsp;
} Stereo3DContext;

#define OFFSET(x) offsetof(Stereo3DContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption stereo3d_options[] = {
    { "in",    "set input format",  OFFSET(in.format),   AV_OPT_TYPE_INT,   {.i64=SIDE_BY_SIDE}, INTERLEAVE_ROWS, STEREO_CODE_COUNT-1, FLAGS, .unit = "in"},
    { "ab2",   "above below half height",             0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW_2},      0, 0, FLAGS, .unit = "in" },
    { "tb2",   "above below half height",             0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW_2},      0, 0, FLAGS, .unit = "in" },
    { "ab",    "above below",                         0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW},        0, 0, FLAGS, .unit = "in" },
    { "tb",    "above below",                         0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW},        0, 0, FLAGS, .unit = "in" },
    { "a",     "alternating frames",                  0, AV_OPT_TYPE_CONST, {.i64=ALTERNATING},        0, 0, FLAGS, .unit = "in" },
    { "alter", "alternating frames",                  0, AV_OPT_TYPE_CONST, {.i64=ALTERNATING},        0, 0, FLAGS, .unit = "in" },
    { "sbs2",  "side by side half width",             0, AV_OPT_TYPE_CONST, {.i64=SIDE_BY_SIDE_2},     0, 0, FLAGS, .unit = "in" },
    { "sbs",   "side by side",                        0, AV_OPT_TYPE_CONST, {.i64=SIDE_BY_SIDE},       0, 0, FLAGS, .unit = "in" },
    { "ir",    "interleave rows",                     0, AV_OPT_TYPE_CONST, {.i64=INTERLEAVE_ROWS},    0, 0, FLAGS, .unit = "in" },
    { "ic",    "interleave columns",                  0, AV_OPT_TYPE_CONST, {.i64=INTERLEAVE_COLS},    0, 0, FLAGS, .unit = "in" },
    { "hdmi",  "HDMI frame pack",                     0, AV_OPT_TYPE_CONST, {.i64=HDMI},               0, 0, FLAGS, .unit = "in" },
    { "out",   "set output format", OFFSET(out.format),  AV_OPT_TYPE_INT,   {.i64=ANAGLYPH_RC_DUBOIS}, 0, STEREO_CODE_COUNT-1, FLAGS, .unit = "out"},
    { "ab2",   "above below half height",             0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW_2},      0, 0, FLAGS, .unit = "out" },
    { "tb2",   "above below half height",             0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW_2},      0, 0, FLAGS, .unit = "out" },
    { "ab",    "above below",                         0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW},        0, 0, FLAGS, .unit = "out" },
    { "tb",    "above below",                         0, AV_OPT_TYPE_CONST, {.i64=ABOVE_BELOW},        0, 0, FLAGS, .unit = "out" },
    { "a",     "alternating frames",                  0, AV_OPT_TYPE_CONST, {.i64=ALTERNATING},        0, 0, FLAGS, .unit = "out" },
    { "agmc",  "anaglyph green magenta color",        0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_GM_COLOR},  0, 0, FLAGS, .unit = "out" },
    { "agmd",  "anaglyph green magenta dubois",       0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_GM_DUBOIS}, 0, 0, FLAGS, .unit = "out" },
    { "agmg",  "anaglyph green magenta gray",         0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_GM_GRAY},   0, 0, FLAGS, .unit = "out" },
    { "agmh",  "anaglyph green magenta half color",   0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_GM_HALF},   0, 0, FLAGS, .unit = "out" },
    { "arbg",  "anaglyph red blue gray",              0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_RB_GRAY},   0, 0, FLAGS, .unit = "out" },
    { "arcc",  "anaglyph red cyan color",             0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_RC_COLOR},  0, 0, FLAGS, .unit = "out" },
    { "arcd",  "anaglyph red cyan dubois",            0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_RC_DUBOIS}, 0, 0, FLAGS, .unit = "out" },
    { "arcg",  "anaglyph red cyan gray",              0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_RC_GRAY},   0, 0, FLAGS, .unit = "out" },
    { "arch",  "anaglyph red cyan half color",        0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_RC_HALF},   0, 0, FLAGS, .unit = "out" },
    { "argg",  "anaglyph red green gray",             0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_RG_GRAY},   0, 0, FLAGS, .unit = "out" },
    { "aybc",  "anaglyph yellow blue color",          0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_YB_COLOR},  0, 0, FLAGS, .unit = "out" },
    { "aybd",  "anaglyph yellow blue dubois",         0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_YB_DUBOIS}, 0, 0, FLAGS, .unit = "out" },
    { "aybg",  "anaglyph yellow blue gray",           0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_YB_GRAY},   0, 0, FLAGS, .unit = "out" },
    { "aybh",  "anaglyph yellow blue half color",     0, AV_OPT_TYPE_CONST, {.i64=ANAGLYPH_YB_HALF},   0, 0, FLAGS, .unit = "out" },
    { "ir",    "interleave rows",                     0, AV_OPT_TYPE_CONST, {.i64=INTERLEAVE_ROWS},    0, 0, FLAGS, .unit = "out" },
    { "m",     "mono",                                0, AV_OPT_TYPE_CONST, {.i64=MONO},               0, 0, FLAGS, .unit = "out" },
    { "mono",  "mono",                                0, AV_OPT_TYPE_CONST, {.i64=MONO},               0, 0, FLAGS, .unit = "out" },
    { "sbs2",  "side by side half width",             0, AV_OPT_TYPE_CONST, {.i64=SIDE_BY_SIDE_2},     0, 0, FLAGS, .unit = "out" },
    { "sbs",   "side by side",                        0, AV_OPT_TYPE_CONST, {.i64=SIDE_BY_SIDE},       0, 0, FLAGS, .unit = "out" },
    { "ch",    "checkerboard",                        0, AV_OPT_TYPE_CONST, {.i64=CHECKERBOARD},       0, 0, FLAGS, .unit = "out" },
    { "ic",    "interleave columns",                  0, AV_OPT_TYPE_CONST, {.i64=INTERLEAVE_COLS},    0, 0, FLAGS, .unit = "out" },
    { "hdmi",  "HDMI frame pack",                     0, AV_OPT_TYPE_CONST, {.i64=HDMI},               0, 0, FLAGS, .unit = "out" },
    { "ineye", "set first input eye",     OFFSET(in.eyes), AV_OPT_TYPE_INT, {.i64=LEFT},0,EYES_CODE_COUNT-1, FLAGS, .unit = "eye" },
    { "left",  "first left",                          0, AV_OPT_TYPE_CONST, {.i64=LEFT},               0, 0, FLAGS, .unit = "eye" },
    { "l",     "first left",                          0, AV_OPT_TYPE_CONST, {.i64=LEFT},               0, 0, FLAGS, .unit = "eye" },
    { "right", "first right",                         0, AV_OPT_TYPE_CONST, {.i64=RIGHT},              0, 0, FLAGS, .unit = "eye" },
    { "r",     "first right",                         0, AV_OPT_TYPE_CONST, {.i64=RIGHT},              0, 0, FLAGS, .unit = "eye" },
    { "outeye", "set first output eye",  OFFSET(out.eyes), AV_OPT_TYPE_INT, {.i64=LEFT},0,EYES_CODE_COUNT-1, FLAGS, .unit = "eye" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(stereo3d);

static const enum AVPixelFormat anaglyph_pix_fmts[] = {
    AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat other_pix_fmts[] = {
    AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24,
    AV_PIX_FMT_RGB48BE, AV_PIX_FMT_BGR48BE,
    AV_PIX_FMT_RGB48LE, AV_PIX_FMT_BGR48LE,
    AV_PIX_FMT_RGBA64BE, AV_PIX_FMT_BGRA64BE,
    AV_PIX_FMT_RGBA64LE, AV_PIX_FMT_BGRA64LE,
    AV_PIX_FMT_RGBA,  AV_PIX_FMT_BGRA,
    AV_PIX_FMT_ARGB,  AV_PIX_FMT_ABGR,
    AV_PIX_FMT_RGB0,  AV_PIX_FMT_BGR0,
    AV_PIX_FMT_0RGB,  AV_PIX_FMT_0BGR,
    AV_PIX_FMT_GBRP,
    AV_PIX_FMT_GBRP9BE,  AV_PIX_FMT_GBRP9LE,
    AV_PIX_FMT_GBRP10BE, AV_PIX_FMT_GBRP10LE,
    AV_PIX_FMT_GBRP12BE, AV_PIX_FMT_GBRP12LE,
    AV_PIX_FMT_GBRP14BE, AV_PIX_FMT_GBRP14LE,
    AV_PIX_FMT_GBRP16BE, AV_PIX_FMT_GBRP16LE,
    AV_PIX_FMT_GBRAP,
    AV_PIX_FMT_GBRAP10BE, AV_PIX_FMT_GBRAP10LE,
    AV_PIX_FMT_GBRAP12BE, AV_PIX_FMT_GBRAP12LE,
    AV_PIX_FMT_GBRAP14BE, AV_PIX_FMT_GBRAP14LE,
    AV_PIX_FMT_GBRAP16BE, AV_PIX_FMT_GBRAP16LE,
    AV_PIX_FMT_YUV410P,
    AV_PIX_FMT_YUV411P,
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVA422P,
    AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_YUVJ411P,
    AV_PIX_FMT_YUVJ420P,
    AV_PIX_FMT_YUVJ422P,
    AV_PIX_FMT_YUVJ440P,
    AV_PIX_FMT_YUVJ444P,
    AV_PIX_FMT_YUV420P9LE,  AV_PIX_FMT_YUVA420P9LE,
    AV_PIX_FMT_YUV420P9BE,  AV_PIX_FMT_YUVA420P9BE,
    AV_PIX_FMT_YUV422P9LE,  AV_PIX_FMT_YUVA422P9LE,
    AV_PIX_FMT_YUV422P9BE,  AV_PIX_FMT_YUVA422P9BE,
    AV_PIX_FMT_YUV444P9LE,  AV_PIX_FMT_YUVA444P9LE,
    AV_PIX_FMT_YUV444P9BE,  AV_PIX_FMT_YUVA444P9BE,
    AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUVA420P10LE,
    AV_PIX_FMT_YUV420P10BE, AV_PIX_FMT_YUVA420P10BE,
    AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_YUVA422P10LE,
    AV_PIX_FMT_YUV422P10BE, AV_PIX_FMT_YUVA422P10BE,
    AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_YUVA444P10LE,
    AV_PIX_FMT_YUV444P10BE, AV_PIX_FMT_YUVA444P10BE,
    AV_PIX_FMT_YUV420P12BE,  AV_PIX_FMT_YUV420P12LE,
    AV_PIX_FMT_YUV422P12BE,  AV_PIX_FMT_YUV422P12LE,
    AV_PIX_FMT_YUV444P12BE,  AV_PIX_FMT_YUV444P12LE,
    AV_PIX_FMT_YUV420P14BE,  AV_PIX_FMT_YUV420P14LE,
    AV_PIX_FMT_YUV422P14BE,  AV_PIX_FMT_YUV422P14LE,
    AV_PIX_FMT_YUV444P14BE,  AV_PIX_FMT_YUV444P14LE,
    AV_PIX_FMT_YUV420P16LE, AV_PIX_FMT_YUVA420P16LE,
    AV_PIX_FMT_YUV420P16BE, AV_PIX_FMT_YUVA420P16BE,
    AV_PIX_FMT_YUV422P16LE, AV_PIX_FMT_YUVA422P16LE,
    AV_PIX_FMT_YUV422P16BE, AV_PIX_FMT_YUVA422P16BE,
    AV_PIX_FMT_YUV444P16LE, AV_PIX_FMT_YUVA444P16LE,
    AV_PIX_FMT_YUV444P16BE, AV_PIX_FMT_YUVA444P16BE,
    AV_PIX_FMT_NONE
};

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const Stereo3DContext *s = ctx->priv;
    const enum AVPixelFormat *pix_fmts;

    switch (s->out.format) {
    case ANAGLYPH_GM_COLOR:
    case ANAGLYPH_GM_DUBOIS:
    case ANAGLYPH_GM_GRAY:
    case ANAGLYPH_GM_HALF:
    case ANAGLYPH_RB_GRAY:
    case ANAGLYPH_RC_COLOR:
    case ANAGLYPH_RC_DUBOIS:
    case ANAGLYPH_RC_GRAY:
    case ANAGLYPH_RC_HALF:
    case ANAGLYPH_RG_GRAY:
    case ANAGLYPH_YB_COLOR:
    case ANAGLYPH_YB_DUBOIS:
    case ANAGLYPH_YB_GRAY:
    case ANAGLYPH_YB_HALF:
        pix_fmts = anaglyph_pix_fmts;
        break;
    default:
        pix_fmts = other_pix_fmts;
    }

    return ff_set_common_formats_from_list2(ctx, cfg_in, cfg_out, pix_fmts);
}

static inline uint8_t ana_convert(const int *coeff, const uint8_t *left, const uint8_t *right)
{
    int sum;

    sum  = coeff[0] * left[0] + coeff[3] * right[0]; //red in
    sum += coeff[1] * left[1] + coeff[4] * right[1]; //green in
    sum += coeff[2] * left[2] + coeff[5] * right[2]; //blue in

    return av_clip_uint8(sum >> 16);
}

static void anaglyph_ic(uint8_t *dst, uint8_t *lsrc, uint8_t *rsrc,
                        ptrdiff_t dst_linesize, ptrdiff_t l_linesize, ptrdiff_t r_linesize,
                        int width, int height,
                        const int *ana_matrix_r, const int *ana_matrix_g, const int *ana_matrix_b)
{
    int x, y, o;

    for (y = 0; y < height; y++) {
        for (o = 0, x = 0; x < width; x++, o+= 3) {
            dst[o    ] = ana_convert(ana_matrix_r, lsrc + o * 2, rsrc + o * 2);
            dst[o + 1] = ana_convert(ana_matrix_g, lsrc + o * 2, rsrc + o * 2);
            dst[o + 2] = ana_convert(ana_matrix_b, lsrc + o * 2, rsrc + o * 2);
        }

        dst  += dst_linesize;
        lsrc += l_linesize;
        rsrc += r_linesize;
    }
}

static void anaglyph(uint8_t *dst, uint8_t *lsrc, uint8_t *rsrc,
                     ptrdiff_t dst_linesize, ptrdiff_t l_linesize, ptrdiff_t r_linesize,
                     int width, int height,
                     const int *ana_matrix_r, const int *ana_matrix_g, const int *ana_matrix_b)
{
    int x, y, o;

    for (y = 0; y < height; y++) {
        for (o = 0, x = 0; x < width; x++, o+= 3) {
            dst[o    ] = ana_convert(ana_matrix_r, lsrc + o, rsrc + o);
            dst[o + 1] = ana_convert(ana_matrix_g, lsrc + o, rsrc + o);
            dst[o + 2] = ana_convert(ana_matrix_b, lsrc + o, rsrc + o);
        }

        dst  += dst_linesize;
        lsrc += l_linesize;
        rsrc += r_linesize;
    }
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    Stereo3DContext *s = ctx->priv;
    FilterLink *il = ff_filter_link(inlink);
    FilterLink *ol = ff_filter_link(outlink);
    AVRational fps = il->frame_rate;
    AVRational tb = inlink->time_base;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(outlink->format);
    int ret;
    s->aspect = inlink->sample_aspect_ratio;

    switch (s->in.format) {
    case INTERLEAVE_COLS:
    case SIDE_BY_SIDE_2:
    case SIDE_BY_SIDE:
        if (inlink->w & 1) {
            av_log(ctx, AV_LOG_ERROR, "width must be even\n");
            return AVERROR_INVALIDDATA;
        }
        break;
    case INTERLEAVE_ROWS:
    case ABOVE_BELOW_2:
    case ABOVE_BELOW:
        if (inlink->h & 1) {
            av_log(ctx, AV_LOG_ERROR, "height must be even\n");
            return AVERROR_INVALIDDATA;
        }
        break;
    }

    s->in.width     =
    s->width        = inlink->w;
    s->in.height    =
    s->height       = inlink->h;
    s->in.off_lstep =
    s->in.off_rstep =
    s->in.off_left  =
    s->in.off_right =
    s->in.row_left  =
    s->in.row_right = 0;
    s->in.row_step  = 1;

    switch (s->in.format) {
    case SIDE_BY_SIDE_2:
        s->aspect.num *= 2;
    case SIDE_BY_SIDE:
        s->width = inlink->w / 2;
        switch (s->in.eyes) {
        case LEFT:
            s->in.off_right = s->width;
            break;
        case RIGHT:
            s->in.off_left = s->width;
            break;
        }
        break;
    case ABOVE_BELOW_2:
        s->aspect.den *= 2;
    case ABOVE_BELOW:
        s->height = inlink->h / 2;
        switch (s->in.eyes) {
        case LEFT:
            s->in.row_right = s->height;
            break;
        case RIGHT:
            s->in.row_left = s->height;
            break;
        }
        break;
    case ALTERNATING:
        fps.den *= 2;
        tb.num *= 2;
        break;
    case INTERLEAVE_COLS:
        s->width = inlink->w / 2;
        break;
    case INTERLEAVE_ROWS:
        s->in.row_step = 2;
        switch (s->in.eyes) {
        case LEFT:
            s->in.off_rstep = 1;
            break;
        case RIGHT:
            s->in.off_lstep = 1;
            break;
        }
        if (s->out.format != CHECKERBOARD)
            s->height = inlink->h / 2;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "input format %d is not supported\n", s->in.format);
        return AVERROR(EINVAL);
    }

    s->out.width     = s->width;
    s->out.height    = s->height;
    s->out.off_lstep =
    s->out.off_rstep =
    s->out.off_left  =
    s->out.off_right =
    s->out.row_left  =
    s->out.row_right = 0;
    s->out.row_step  = 1;

    switch (s->out.format) {
    case ANAGLYPH_RB_GRAY:
    case ANAGLYPH_RG_GRAY:
    case ANAGLYPH_RC_GRAY:
    case ANAGLYPH_RC_HALF:
    case ANAGLYPH_RC_COLOR:
    case ANAGLYPH_RC_DUBOIS:
    case ANAGLYPH_GM_GRAY:
    case ANAGLYPH_GM_HALF:
    case ANAGLYPH_GM_COLOR:
    case ANAGLYPH_GM_DUBOIS:
    case ANAGLYPH_YB_GRAY:
    case ANAGLYPH_YB_HALF:
    case ANAGLYPH_YB_COLOR:
    case ANAGLYPH_YB_DUBOIS: {
        uint8_t rgba_map[4];

        ff_fill_rgba_map(rgba_map, outlink->format);
        s->ana_matrix[rgba_map[0]] = &ana_coeff[s->out.format][0][0];
        s->ana_matrix[rgba_map[1]] = &ana_coeff[s->out.format][1][0];
        s->ana_matrix[rgba_map[2]] = &ana_coeff[s->out.format][2][0];
        break;
    }
    case SIDE_BY_SIDE_2:
        s->aspect.den *= 2;
    case SIDE_BY_SIDE:
        s->out.width = s->width * 2;
        s->out.off_right = s->width;
        switch (s->out.eyes) {
        case LEFT:
            s->out.off_right = s->width;
            break;
        case RIGHT:
            s->out.off_left = s->width;
            break;
        }
        break;
    case ABOVE_BELOW_2:
        s->aspect.num *= 2;
    case ABOVE_BELOW:
        s->out.height = s->height * 2;
        switch (s->out.eyes) {
        case LEFT:
            s->out.row_right = s->height;
            break;
        case RIGHT:
            s->out.row_left = s->height;
            break;
        }
        break;
    case HDMI:
        if (s->height != 720 && s->height != 1080)
            av_log(ctx, AV_LOG_WARNING, "Only 720 and 1080 height supported\n");

        s->blanks = s->height / 24;
        s->out.height    = s->height * 2 + s->blanks;
        s->out.row_right = s->height + s->blanks;
        break;
    case INTERLEAVE_ROWS:
        s->out.row_step  = 2;
        s->out.height    = s->height * 2;
        switch (s->out.eyes) {
        case LEFT:
            s->in.row_step   = 1 + (s->in.format == INTERLEAVE_ROWS && s->in.eyes == RIGHT);
            s->out.off_rstep = 1;
            break;
        case RIGHT:
            s->in.row_step   = 1 + (s->in.format == INTERLEAVE_ROWS && s->in.eyes == LEFT);
            s->out.off_lstep = 1;
            break;
        }
        break;
    case MONO:
        if (s->in.format != INTERLEAVE_COLS && s->in.eyes != s->out.eyes) {
            s->in.off_left = s->in.off_right;
            s->in.row_left = s->in.row_right;
        }
        if (s->in.format == INTERLEAVE_ROWS && s->in.eyes != s->out.eyes)
            FFSWAP(int, s->in.off_lstep, s->in.off_rstep);
        break;
    case ALTERNATING:
        fps.num *= 2;
        tb.den *= 2;
        break;
    case CHECKERBOARD:
    case INTERLEAVE_COLS:
        s->out.width = s->width * 2;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "output format %d is not supported\n", s->out.format);
        return AVERROR(EINVAL);
    }

    if (s->in.format == INTERLEAVE_COLS) {
        if (s->in.eyes != s->out.eyes) {
            FFSWAP(int, s->in.row_left,   s->in.row_right);
            FFSWAP(int, s->in.off_lstep,  s->in.off_rstep);
            FFSWAP(int, s->in.off_left,   s->in.off_right);
            FFSWAP(int, s->out.row_left,  s->out.row_right);
            FFSWAP(int, s->out.off_lstep, s->out.off_rstep);
            FFSWAP(int, s->out.off_left,  s->out.off_right);
        }
    }

    outlink->w = s->out.width;
    outlink->h = s->out.height;
    ol->frame_rate = fps;
    outlink->time_base = tb;
    outlink->sample_aspect_ratio = s->aspect;

    if ((ret = av_image_fill_linesizes(s->linesize, outlink->format, s->width)) < 0)
        return ret;
    s->nb_planes = av_pix_fmt_count_planes(outlink->format);
    av_image_fill_max_pixsteps(s->pixstep, NULL, desc);
    s->pheight[1] = s->pheight[2] = AV_CEIL_RSHIFT(s->height, desc->log2_chroma_h);
    s->pheight[0] = s->pheight[3] = s->height;
    s->hsub = desc->log2_chroma_w;
    s->vsub = desc->log2_chroma_h;

    s->dsp.anaglyph = anaglyph;
#if ARCH_X86
    ff_stereo3d_init_x86(&s->dsp);
#endif

    return 0;
}

typedef struct ThreadData {
    AVFrame *ileft, *iright;
    AVFrame *out;
} ThreadData;

static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    Stereo3DContext *s = ctx->priv;
    ThreadData *td = arg;
    AVFrame *ileft = td->ileft;
    AVFrame *iright = td->iright;
    AVFrame *out = td->out;
    int height = s->out.height;
    int start = (height *  jobnr   ) / nb_jobs;
    int end   = (height * (jobnr+1)) / nb_jobs;
    const int **ana_matrix = s->ana_matrix;

    s->dsp.anaglyph(out->data[0] + out->linesize[0] * start,
             ileft ->data[0] + s->in_off_left [0]  + ileft->linesize[0] * start * s->in.row_step,
             iright->data[0] + s->in_off_right[0] + iright->linesize[0] * start * s->in.row_step,
             out->linesize[0],
             ileft->linesize[0] * s->in.row_step,
             iright->linesize[0] * s->in.row_step,
             s->out.width, end - start,
             ana_matrix[0], ana_matrix[1], ana_matrix[2]);

    return 0;
}

static void interleave_cols_to_any(Stereo3DContext *s, int *out_off, int p, AVFrame *in, AVFrame *out, int d)
{
    for (int y = 0; y < s->pheight[p]; y++) {
        const uint8_t *src = (const uint8_t*)in->data[p] + y * in->linesize[p] + d * s->pixstep[p];
        uint8_t *dst = out->data[p] + out_off[p] + y * out->linesize[p] * s->out.row_step;
        const int linesize = s->linesize[p];

        switch (s->pixstep[p]) {
        case 1:
            for (int x = 0; x < linesize; x++)
                dst[x] = src[x * 2];
            break;
        case 2:
            for (int x = 0; x < linesize; x+=2)
                AV_WN16(&dst[x], AV_RN16(&src[x * 2]));
            break;
        case 3:
            for (int x = 0; x < linesize; x+=3)
                AV_WB24(&dst[x], AV_RB24(&src[x * 2]));
            break;
        case 4:
            for (int x = 0; x < linesize; x+=4)
                AV_WN32(&dst[x], AV_RN32(&src[x * 2]));
            break;
        case 6:
            for (int x = 0; x < linesize; x+=6)
                AV_WB48(&dst[x], AV_RB48(&src[x * 2]));
            break;
        case 8:
            for (int x = 0; x < linesize; x+=8)
                AV_WN64(&dst[x], AV_RN64(&src[x * 2]));
            break;
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *inpicref)
{
    AVFilterContext *ctx  = inlink->dst;
    Stereo3DContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out = NULL, *oleft, *oright, *ileft, *iright;
    int out_off_left[4], out_off_right[4], ret;

    if (s->in.format == s->out.format &&
        s->in.eyes == s->out.eyes)
        return ff_filter_frame(outlink, inpicref);

    switch (s->out.format) {
    case ALTERNATING:
        if (!s->prev) {
            s->prev = inpicref;
            return 0;
        }
        break;
    };

    switch (s->in.format) {
    case ALTERNATING:
        if (!s->prev) {
            s->prev = inpicref;
            return 0;
        }
        ileft  = s->prev;
        iright = inpicref;
        if (s->in.eyes != s->out.eyes)
            FFSWAP(AVFrame *, ileft, iright);
        break;
    default:
        ileft = iright = inpicref;
    };

    if (s->out.format == ALTERNATING &&
        (s->in.format == SIDE_BY_SIDE ||
         s->in.format == SIDE_BY_SIDE_2 ||
         s->in.format == ABOVE_BELOW ||
         s->in.format == ABOVE_BELOW_2 ||
         s->in.format == INTERLEAVE_ROWS)) {
        oright = av_frame_clone(s->prev);
        oleft  = av_frame_clone(s->prev);
        if (!oright || !oleft) {
            av_frame_free(&oright);
            av_frame_free(&oleft);
            av_frame_free(&s->prev);
            av_frame_free(&inpicref);
            return AVERROR(ENOMEM);
        }
    } else if (s->out.format == MONO &&
        (s->in.format == SIDE_BY_SIDE ||
         s->in.format == SIDE_BY_SIDE_2 ||
         s->in.format == ABOVE_BELOW ||
         s->in.format == ABOVE_BELOW_2 ||
         s->in.format == INTERLEAVE_ROWS)) {
        out = oleft = oright = av_frame_clone(inpicref);
        if (!out) {
            av_frame_free(&s->prev);
            av_frame_free(&inpicref);
            return AVERROR(ENOMEM);
        }
    } else if ((s->out.format == MONO && s->in.format == ALTERNATING) &&
               (s->out.eyes == s->in.eyes)) {
        s->prev->pts /= 2;
        ret = ff_filter_frame(outlink, s->prev);
        av_frame_free(&inpicref);
        s->prev = NULL;
        return ret;
    } else if ((s->out.format == MONO && s->in.format == ALTERNATING) &&
               (s->out.eyes != s->in.eyes)) {
        av_frame_free(&s->prev);
        inpicref->pts /= 2;
        return ff_filter_frame(outlink, inpicref);
    } else if ((s->out.format == ALTERNATING && s->in.format == ALTERNATING) &&
               (s->out.eyes != s->in.eyes)) {
        FFSWAP(int64_t, s->prev->pts, inpicref->pts);
        ff_filter_frame(outlink, inpicref);
        ret = ff_filter_frame(outlink, s->prev);
        s->prev = NULL;
        return ret;
    } else {
        out = oleft = oright = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&s->prev);
            av_frame_free(&inpicref);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, inpicref);

        if (s->out.format == ALTERNATING) {
            oright = ff_get_video_buffer(outlink, outlink->w, outlink->h);
            if (!oright) {
                av_frame_free(&oleft);
                av_frame_free(&s->prev);
                av_frame_free(&inpicref);
                return AVERROR(ENOMEM);
            }
            av_frame_copy_props(oright, s->prev);
        }
    }

    for (int i = 0; i < 4; i++) {
        int hsub = i == 1 || i == 2 ? s->hsub : 0;
        int vsub = i == 1 || i == 2 ? s->vsub : 0;
        s->in_off_left[i]   = (AV_CEIL_RSHIFT(s->in.row_left,   vsub) + s->in.off_lstep)  * ileft->linesize[i]  + AV_CEIL_RSHIFT(s->in.off_left   * s->pixstep[i], hsub);
        s->in_off_right[i]  = (AV_CEIL_RSHIFT(s->in.row_right,  vsub) + s->in.off_rstep)  * iright->linesize[i] + AV_CEIL_RSHIFT(s->in.off_right  * s->pixstep[i], hsub);
        out_off_left[i]  = (AV_CEIL_RSHIFT(s->out.row_left,  vsub) + s->out.off_lstep) * oleft->linesize[i]  + AV_CEIL_RSHIFT(s->out.off_left  * s->pixstep[i], hsub);
        out_off_right[i] = (AV_CEIL_RSHIFT(s->out.row_right, vsub) + s->out.off_rstep) * oright->linesize[i] + AV_CEIL_RSHIFT(s->out.off_right * s->pixstep[i], hsub);
    }

    switch (s->out.format) {
    case ALTERNATING:
        switch (s->in.format) {
        case INTERLEAVE_ROWS:
            for (int i = 0; i < s->nb_planes; i++) {
                oleft->linesize[i]  *= 2;
                oright->linesize[i] *= 2;
            }
        case ABOVE_BELOW:
        case ABOVE_BELOW_2:
        case SIDE_BY_SIDE:
        case SIDE_BY_SIDE_2:
            oleft->width   = outlink->w;
            oright->width  = outlink->w;
            oleft->height  = outlink->h;
            oright->height = outlink->h;

            for (int i = 0; i < s->nb_planes; i++) {
                oleft->data[i]  += s->in_off_left[i];
                oright->data[i] += s->in_off_right[i];
            }
            break;
        default:
            goto copy;
            break;
        }
        break;
    case HDMI:
        for (int i = 0; i < s->nb_planes; i++) {
            int h = s->height >> ((i == 1 || i == 2) ? s->vsub : 0);
            int b = (s->blanks) >> ((i == 1 || i == 2) ? s->vsub : 0);

            for (int j = h; j < h + b; j++)
                memset(oleft->data[i] + j * s->linesize[i], 0, s->linesize[i]);
        }
    case SIDE_BY_SIDE:
    case SIDE_BY_SIDE_2:
    case ABOVE_BELOW:
    case ABOVE_BELOW_2:
    case INTERLEAVE_ROWS:
copy:
        if (s->in.format == INTERLEAVE_COLS) {
            for (int i = 0; i < s->nb_planes; i++) {
                int d = s->in.eyes != s->out.eyes;

                interleave_cols_to_any(s, out_off_left,  i, ileft,  oleft,   d);
                interleave_cols_to_any(s, out_off_right, i, iright, oright, !d);
            }
        } else {
            for (int i = 0; i < s->nb_planes; i++) {
                av_image_copy_plane(oleft->data[i] + out_off_left[i],
                                    oleft->linesize[i] * s->out.row_step,
                                    ileft->data[i] + s->in_off_left[i],
                                    ileft->linesize[i] * s->in.row_step,
                                    s->linesize[i], s->pheight[i]);
                av_image_copy_plane(oright->data[i] + out_off_right[i],
                                    oright->linesize[i] * s->out.row_step,
                                    iright->data[i] + s->in_off_right[i],
                                    iright->linesize[i] * s->in.row_step,
                                    s->linesize[i], s->pheight[i]);
            }
        }
        break;
    case MONO:
        if (s->out.eyes == LEFT)
            iright = ileft;

        switch (s->in.format) {
        case INTERLEAVE_ROWS:
            for (int i = 0; i < s->nb_planes; i++)
                out->linesize[i] *= 2;
        case ABOVE_BELOW:
        case ABOVE_BELOW_2:
        case SIDE_BY_SIDE:
        case SIDE_BY_SIDE_2:
            out->width  = outlink->w;
            out->height = outlink->h;

            if (s->in.eyes == s->out.eyes) {
                for (int i = 0; i < s->nb_planes; i++)
                    out->data[i] += s->in_off_left[i];
            } else if (s->in.eyes != s->out.eyes) {
                for (int i = 0; i < s->nb_planes; i++)
                    out->data[i] += s->in_off_right[i];
            }
            break;
        case INTERLEAVE_COLS:
            for (int i = 0; i < s->nb_planes; i++) {
                const int d = s->in.eyes != s->out.eyes;

                interleave_cols_to_any(s, out_off_right, i, iright, out, d);
            }
            break;
        default:
            for (int i = 0; i < s->nb_planes; i++) {
                av_image_copy_plane(out->data[i], out->linesize[i],
                                    iright->data[i] + s->in_off_left[i],
                                    iright->linesize[i] * s->in.row_step,
                                    s->linesize[i], s->pheight[i]);
            }
            break;
        }
        break;
    case ANAGLYPH_RB_GRAY:
    case ANAGLYPH_RG_GRAY:
    case ANAGLYPH_RC_GRAY:
    case ANAGLYPH_RC_HALF:
    case ANAGLYPH_RC_COLOR:
    case ANAGLYPH_RC_DUBOIS:
    case ANAGLYPH_GM_GRAY:
    case ANAGLYPH_GM_HALF:
    case ANAGLYPH_GM_COLOR:
    case ANAGLYPH_GM_DUBOIS:
    case ANAGLYPH_YB_GRAY:
    case ANAGLYPH_YB_HALF:
    case ANAGLYPH_YB_COLOR:
    case ANAGLYPH_YB_DUBOIS:
        if (s->in.format == INTERLEAVE_COLS) {
            const int d = s->in.eyes != s->out.eyes;

            anaglyph_ic(out->data[0],
                ileft ->data[0] + s->in_off_left [0] +   d  * 3,
                iright->data[0] + s->in_off_right[0] + (!d) * 3,
                out->linesize[0],
                ileft->linesize[0] * s->in.row_step,
                iright->linesize[0] * s->in.row_step,
                s->out.width, s->out.height,
                s->ana_matrix[0], s->ana_matrix[1], s->ana_matrix[2]);
        } else {
            ThreadData td;

            td.ileft = ileft; td.iright = iright; td.out = out;
            ff_filter_execute(ctx, filter_slice, &td, NULL,
                              FFMIN(s->out.height, ff_filter_get_nb_threads(ctx)));
        }
        break;
    case CHECKERBOARD:
        for (int i = 0; i < s->nb_planes; i++) {
            for (int y = 0; y < s->pheight[i]; y++) {
                uint8_t *dst = out->data[i] + out->linesize[i] * y;
                const int d1 = (s->in.format == INTERLEAVE_COLS) && (s->in.eyes != s->out.eyes);
                const int d2 = (s->in.format == INTERLEAVE_COLS) ? !d1 : 0;
                const int m = 1 + (s->in.format == INTERLEAVE_COLS);
                uint8_t *left  = ileft->data[i]  + ileft->linesize[i]  * y + s->in_off_left[i]  + d1 * s->pixstep[i];
                uint8_t *right = iright->data[i] + iright->linesize[i] * y + s->in_off_right[i] + d2 * s->pixstep[i];
                const int linesize = s->linesize[i] * 2;

                if (s->out.format == CHECKERBOARD && s->out.eyes == RIGHT && s->in.format != INTERLEAVE_COLS)
                    FFSWAP(uint8_t*, left, right);
                switch (s->pixstep[i]) {
                case 1:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=2, p++, b+=2) {
                        dst[x  ] = (b&1) == (y&1) ? left[p*m] : right[p*m];
                        dst[x+1] = (b&1) != (y&1) ? left[p*m] : right[p*m];
                    }
                    break;
                case 2:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=4, p+=2, b+=2) {
                        AV_WN16(&dst[x  ], (b&1) == (y&1) ? AV_RN16(&left[p*m]) : AV_RN16(&right[p*m]));
                        AV_WN16(&dst[x+2], (b&1) != (y&1) ? AV_RN16(&left[p*m]) : AV_RN16(&right[p*m]));
                    }
                    break;
                case 3:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=6, p+=3, b+=2) {
                        AV_WB24(&dst[x  ], (b&1) == (y&1) ? AV_RB24(&left[p*m]) : AV_RB24(&right[p*m]));
                        AV_WB24(&dst[x+3], (b&1) != (y&1) ? AV_RB24(&left[p*m]) : AV_RB24(&right[p*m]));
                    }
                    break;
                case 4:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=8, p+=4, b+=2) {
                        AV_WN32(&dst[x  ], (b&1) == (y&1) ? AV_RN32(&left[p*m]) : AV_RN32(&right[p*m]));
                        AV_WN32(&dst[x+4], (b&1) != (y&1) ? AV_RN32(&left[p*m]) : AV_RN32(&right[p*m]));
                    }
                    break;
                case 6:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=12, p+=6, b+=2) {
                        AV_WB48(&dst[x  ], (b&1) == (y&1) ? AV_RB48(&left[p*m]) : AV_RB48(&right[p*m]));
                        AV_WB48(&dst[x+6], (b&1) != (y&1) ? AV_RB48(&left[p*m]) : AV_RB48(&right[p*m]));
                    }
                    break;
                case 8:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=16, p+=8, b+=2) {
                        AV_WN64(&dst[x  ], (b&1) == (y&1) ? AV_RN64(&left[p*m]) : AV_RN64(&right[p*m]));
                        AV_WN64(&dst[x+8], (b&1) != (y&1) ? AV_RN64(&left[p*m]) : AV_RN64(&right[p*m]));
                    }
                    break;
                }
            }
        }
        break;
    case INTERLEAVE_COLS:
        for (int i = 0; i < s->nb_planes; i++) {
            const int d = s->in.format == INTERLEAVE_COLS;
            const int m = 1 + d;

            for (int y = 0; y < s->pheight[i]; y++) {
                uint8_t *dst = out->data[i] + out->linesize[i] * y;
                uint8_t *left = ileft->data[i] + ileft->linesize[i] * y * s->in.row_step + s->in_off_left[i] + d * s->pixstep[i];
                uint8_t *right = iright->data[i] + iright->linesize[i] * y * s->in.row_step + s->in_off_right[i];
                const int linesize = s->linesize[i] * 2;

                if (s->out.format == INTERLEAVE_COLS && s->out.eyes == LEFT)
                    FFSWAP(uint8_t*, left, right);

                switch (s->pixstep[i]) {
                case 1:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=2, p++, b+=2) {
                        dst[x  ] =   b&1  ? left[p*m] : right[p*m];
                        dst[x+1] = !(b&1) ? left[p*m] : right[p*m];
                    }
                    break;
                case 2:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=4, p+=2, b+=2) {
                        AV_WN16(&dst[x  ],   b&1  ? AV_RN16(&left[p*m]) : AV_RN16(&right[p*m]));
                        AV_WN16(&dst[x+2], !(b&1) ? AV_RN16(&left[p*m]) : AV_RN16(&right[p*m]));
                    }
                    break;
                case 3:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=6, p+=3, b+=2) {
                        AV_WB24(&dst[x  ],   b&1  ? AV_RB24(&left[p*m]) : AV_RB24(&right[p*m]));
                        AV_WB24(&dst[x+3], !(b&1) ? AV_RB24(&left[p*m]) : AV_RB24(&right[p*m]));
                    }
                    break;
                case 4:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=8, p+=4, b+=2) {
                        AV_WN32(&dst[x  ],   b&1  ? AV_RN32(&left[p*m]) : AV_RN32(&right[p*m]));
                        AV_WN32(&dst[x+4], !(b&1) ? AV_RN32(&left[p*m]) : AV_RN32(&right[p*m]));
                    }
                    break;
                case 6:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=12, p+=6, b+=2) {
                        AV_WB48(&dst[x  ],   b&1  ? AV_RB48(&left[p*m]) : AV_RB48(&right[p*m]));
                        AV_WB48(&dst[x+6], !(b&1) ? AV_RB48(&left[p*m]) : AV_RB48(&right[p*m]));
                    }
                    break;
                case 8:
                    for (int x = 0, b = 0, p = 0; x < linesize; x+=16, p+=8, b+=2) {
                        AV_WN64(&dst[x  ],   b&1 ?  AV_RN64(&left[p*m]) : AV_RN64(&right[p*m]));
                        AV_WN64(&dst[x+8], !(b&1) ? AV_RN64(&left[p*m]) : AV_RN64(&right[p*m]));
                    }
                    break;
                }
            }
        }
        break;
    default:
        av_assert0(0);
    }

    if (oright != oleft) {
        if (s->out.format == ALTERNATING && s->out.eyes == LEFT)
            FFSWAP(AVFrame *, oleft, oright);
        oright->pts = s->prev->pts * 2;
        ff_filter_frame(outlink, oright);
        out = oleft;
        oleft->pts = s->prev->pts + inpicref->pts;
        av_frame_free(&s->prev);
        s->prev = inpicref;
    } else if (s->in.format == ALTERNATING) {
        out->pts = s->prev->pts / 2;
        av_frame_free(&s->prev);
        av_frame_free(&inpicref);
    } else {
        av_frame_free(&s->prev);
        av_frame_free(&inpicref);
    }
    av_assert0(out);
    out->sample_aspect_ratio = s->aspect;
    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    Stereo3DContext *s = ctx->priv;

    av_frame_free(&s->prev);
}

static const AVFilterPad stereo3d_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad stereo3d_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
};

const FFFilter ff_vf_stereo3d = {
    .p.name        = "stereo3d",
    .p.description = NULL_IF_CONFIG_SMALL("Convert video stereoscopic 3D view."),
    .p.priv_class  = &stereo3d_class,
    .p.flags       = AVFILTER_FLAG_SLICE_THREADS,
    .priv_size     = sizeof(Stereo3DContext),
    .uninit        = uninit,
    FILTER_INPUTS(stereo3d_inputs),
    FILTER_OUTPUTS(stereo3d_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
