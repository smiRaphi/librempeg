/*
 * Copyright (c) 2011 Baptiste Coudurier
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2012 Clément Bœsch
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
 * Libass subtitles burning filter.
 *
 * @see{http://www.matroska.org/technical/specs/subtitles/ssa.html}
 */

#include <ass/ass.h>

#include "config_components.h"
#if CONFIG_SUBTITLES_FILTER
# include "libavcodec/avcodec.h"
# include "libavcodec/codec_desc.h"
# include "libavformat/avformat.h"
#endif
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "filters.h"
#include "drawutils.h"
#include "avfilter.h"
#include "formats.h"
#include "video.h"

#define FF_ASS_FEATURE_WRAP_UNICODE     (LIBASS_VERSION >= 0x01600010)

typedef struct AssContext {
    const AVClass *class;
    ASS_Library  *library;
    ASS_Renderer *renderer;
    ASS_Track    *track;
    char *filename;
    char *fontsdir;
    char *charenc;
    char *force_style;
    int stream_index;
    int alpha;
    uint8_t rgba_map[4];
    int     pix_step[4];       ///< steps per pixel for each plane of the main output
    int original_w, original_h;
    int shaping;
    FFDrawContext draw;
    int wrap_unicode;
} AssContext;

#define OFFSET(x) offsetof(AssContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

#define COMMON_OPTIONS \
    {"filename",       "set the filename of file to read",                         OFFSET(filename),   AV_OPT_TYPE_STRING,     {.str = NULL},  0, 0, FLAGS }, \
    {"f",              "set the filename of file to read",                         OFFSET(filename),   AV_OPT_TYPE_STRING,     {.str = NULL},  0, 0, FLAGS }, \
    {"original_size",  "set the size of the original video (used to scale fonts)", OFFSET(original_w), AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL},  0, 0, FLAGS }, \
    {"fontsdir",       "set the directory containing the fonts to read",           OFFSET(fontsdir),   AV_OPT_TYPE_STRING,     {.str = NULL},  0, 0, FLAGS }, \
    {"alpha",          "enable processing of alpha channel",                       OFFSET(alpha),      AV_OPT_TYPE_BOOL,       {.i64 = 0   },         0,        1, FLAGS }, \

/* libass supports a log level ranging from 0 to 7 */
static const int ass_libavfilter_log_level_map[] = {
    [0] = AV_LOG_FATAL,     /* MSGL_FATAL */
    [1] = AV_LOG_ERROR,     /* MSGL_ERR */
    [2] = AV_LOG_WARNING,   /* MSGL_WARN */
    [3] = AV_LOG_WARNING,   /* <undefined> */
    [4] = AV_LOG_INFO,      /* MSGL_INFO */
    [5] = AV_LOG_INFO,      /* <undefined> */
    [6] = AV_LOG_VERBOSE,   /* MSGL_V */
    [7] = AV_LOG_DEBUG,     /* MSGL_DBG2 */
};

static enum AVColorSpace ass_get_color_space(ASS_YCbCrMatrix ass_matrix, enum AVColorSpace inlink_space) {
    switch (ass_matrix) {
    case YCBCR_NONE:            return inlink_space;
    case YCBCR_SMPTE240M_TV:
    case YCBCR_SMPTE240M_PC:    return AVCOL_SPC_SMPTE240M;
    case YCBCR_FCC_TV:
    case YCBCR_FCC_PC:          return AVCOL_SPC_FCC;
    case YCBCR_BT709_TV:
    case YCBCR_BT709_PC:        return AVCOL_SPC_BT709;
    case YCBCR_BT601_TV:
    case YCBCR_BT601_PC:
    case YCBCR_DEFAULT:
    case YCBCR_UNKNOWN:
    default:                    return AVCOL_SPC_SMPTE170M;
    }
}

static enum AVColorRange ass_get_color_range(ASS_YCbCrMatrix ass_matrix, enum AVColorRange inlink_range) {
    switch (ass_matrix) {
    case YCBCR_NONE:            return inlink_range;
    case YCBCR_SMPTE240M_PC:
    case YCBCR_FCC_PC:
    case YCBCR_BT709_PC:
    case YCBCR_BT601_PC:        return AVCOL_RANGE_JPEG;
    case YCBCR_SMPTE240M_TV:
    case YCBCR_FCC_TV:
    case YCBCR_BT709_TV:
    case YCBCR_BT601_TV:
    case YCBCR_DEFAULT:
    case YCBCR_UNKNOWN:
    default:                    return AVCOL_RANGE_MPEG;
    }
}

static void ass_log(int ass_level, const char *fmt, va_list args, void *ctx)
{
    const int ass_level_clip = av_clip(ass_level, 0,
        FF_ARRAY_ELEMS(ass_libavfilter_log_level_map) - 1);
    const int level = ass_libavfilter_log_level_map[ass_level_clip];

    av_vlog(ctx, level, fmt, args);
    av_log(ctx, level, "\n");
}

static av_cold int init(AVFilterContext *ctx)
{
    AssContext *ass = ctx->priv;

    if (!ass->filename) {
        av_log(ctx, AV_LOG_ERROR, "No filename provided!\n");
        return AVERROR(EINVAL);
    }

    ass->library = ass_library_init();
    if (!ass->library) {
        av_log(ctx, AV_LOG_ERROR, "Could not initialize libass.\n");
        return AVERROR(EINVAL);
    }
    ass_set_message_cb(ass->library, ass_log, ctx);

    ass_set_fonts_dir(ass->library, ass->fontsdir);
    ass_set_extract_fonts(ass->library, 1);

    ass->renderer = ass_renderer_init(ass->library);
    if (!ass->renderer) {
        av_log(ctx, AV_LOG_ERROR, "Could not initialize libass renderer.\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    AssContext *ass = ctx->priv;

    if (ass->track)
        ass_free_track(ass->track);
    if (ass->renderer)
        ass_renderer_done(ass->renderer);
    if (ass->library)
        ass_library_done(ass->library);
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    return ff_set_common_formats2(ctx, cfg_in, cfg_out,
                                  ff_draw_supported_pixel_formats(0));
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    AssContext *ass = ctx->priv;
    int ret;

    ret = ff_draw_init2(&ass->draw, inlink->format,
                        ass_get_color_space(ass->track->YCbCrMatrix, inlink->colorspace),
                        ass_get_color_range(ass->track->YCbCrMatrix, inlink->color_range),
                        ass->alpha ? FF_DRAW_PROCESS_ALPHA : 0);
    if (ret < 0)
        return ret;

    ass_set_frame_size  (ass->renderer, inlink->w, inlink->h);
    if (ass->original_w && ass->original_h) {
        ass_set_pixel_aspect(ass->renderer, (double)inlink->w / inlink->h /
                             ((double)ass->original_w / ass->original_h));
        ass_set_storage_size(ass->renderer, ass->original_w, ass->original_h);
    } else
        ass_set_storage_size(ass->renderer, inlink->w, inlink->h);

    if (ass->shaping != -1)
        ass_set_shaper(ass->renderer, ass->shaping);

    return 0;
}

/* libass stores an RGBA color in the format RRGGBBTT, where TT is the transparency level */
#define AR(c)  ( (c)>>24)
#define AG(c)  (((c)>>16)&0xFF)
#define AB(c)  (((c)>>8) &0xFF)
#define AA(c)  ((0xFF-(c)) &0xFF)

static void overlay_ass_image(AssContext *ass, AVFrame *picref,
                              const ASS_Image *image)
{
    for (; image; image = image->next) {
        uint8_t rgba_color[] = {AR(image->color), AG(image->color), AB(image->color), AA(image->color)};
        FFDrawColor color;
        ff_draw_color(&ass->draw, &color, rgba_color);
        ff_blend_mask(&ass->draw, &color,
                      picref->data, picref->linesize,
                      picref->width, picref->height,
                      image->bitmap, image->stride, image->w, image->h,
                      3, 0, image->dst_x, image->dst_y);
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *picref)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AssContext *ass = ctx->priv;
    int detect_change = 0;
    double time_ms = picref->pts * av_q2d(inlink->time_base) * 1000;
    ASS_Image *image = ass_render_frame(ass->renderer, ass->track,
                                        time_ms, &detect_change);

    if (detect_change)
        av_log(ctx, AV_LOG_DEBUG, "Change happened at time ms:%f\n", time_ms);

    overlay_ass_image(ass, picref, image);

    return ff_filter_frame(outlink, picref);
}

static const AVFilterPad ass_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .flags            = AVFILTERPAD_FLAG_NEEDS_WRITABLE,
        .filter_frame     = filter_frame,
        .config_props     = config_input,
    },
};

#if CONFIG_ASS_FILTER

static const AVOption ass_options[] = {
    COMMON_OPTIONS
    {"shaping", "set shaping engine", OFFSET(shaping), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, 1, FLAGS, .unit = "shaping_mode"},
        {"auto", NULL,                 0, AV_OPT_TYPE_CONST, {.i64 = -1},                  0, 0, FLAGS, .unit = "shaping_mode"},
        {"simple",  "simple shaping",  0, AV_OPT_TYPE_CONST, {.i64 = ASS_SHAPING_SIMPLE},  0, 0, FLAGS, .unit = "shaping_mode"},
        {"complex", "complex shaping", 0, AV_OPT_TYPE_CONST, {.i64 = ASS_SHAPING_COMPLEX}, 0, 0, FLAGS, .unit = "shaping_mode"},
    {NULL},
};

AVFILTER_DEFINE_CLASS(ass);

static av_cold int init_ass(AVFilterContext *ctx)
{
    AssContext *ass = ctx->priv;
    int ret = init(ctx);

    if (ret < 0)
        return ret;

    /* Initialize fonts */
    ass_set_fonts(ass->renderer, NULL, NULL, 1, NULL, 1);

    ass->track = ass_read_file(ass->library, ass->filename, NULL);
    if (!ass->track) {
        av_log(ctx, AV_LOG_ERROR,
               "Could not create a libass track when reading file '%s'\n",
               ass->filename);
        return AVERROR(EINVAL);
    }
    return 0;
}

const FFFilter ff_vf_ass = {
    .p.name        = "ass",
    .p.description = NULL_IF_CONFIG_SMALL("Render ASS subtitles onto input video using the libass library."),
    .p.priv_class  = &ass_class,
    .priv_size     = sizeof(AssContext),
    .init          = init_ass,
    .uninit        = uninit,
    FILTER_INPUTS(ass_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
};
#endif

#if CONFIG_SUBTITLES_FILTER

static const AVOption subtitles_options[] = {
    COMMON_OPTIONS
    {"charenc",      "set input character encoding", OFFSET(charenc),      AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},
    {"stream_index", "set stream index",             OFFSET(stream_index), AV_OPT_TYPE_INT,    { .i64 = -1 }, -1,       INT_MAX,  FLAGS},
    {"si",           "set stream index",             OFFSET(stream_index), AV_OPT_TYPE_INT,    { .i64 = -1 }, -1,       INT_MAX,  FLAGS},
    {"force_style",  "force subtitle style",         OFFSET(force_style),  AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, FLAGS},
#if FF_ASS_FEATURE_WRAP_UNICODE
    {"wrap_unicode", "break lines according to the Unicode Line Breaking Algorithm", OFFSET(wrap_unicode), AV_OPT_TYPE_BOOL, { .i64 = -1 }, -1, 1, FLAGS },
#endif
    {NULL},
};

static const char * const font_mimetypes[] = {
    "font/ttf",
    "font/otf",
    "font/sfnt",
    "font/woff",
    "font/woff2",
    "application/font-sfnt",
    "application/font-woff",
    "application/x-truetype-font",
    "application/vnd.ms-opentype",
    "application/x-font-ttf",
    NULL
};

static int attachment_is_font(AVStream * st)
{
    const AVDictionaryEntry *tag = NULL;
    int n;

    tag = av_dict_get(st->metadata, "mimetype", NULL, AV_DICT_MATCH_CASE);

    if (tag) {
        for (n = 0; font_mimetypes[n]; n++) {
            if (av_strcasecmp(font_mimetypes[n], tag->value) == 0)
                return 1;
        }
    }
    return 0;
}

AVFILTER_DEFINE_CLASS(subtitles);

static av_cold int init_subtitles(AVFilterContext *ctx)
{
    int j, ret, sid;
    int k = 0;
    AVDictionary *codec_opts = NULL;
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *dec;
    const AVCodecDescriptor *dec_desc;
    AVStream *st;
    AVPacket pkt;
    AssContext *ass = ctx->priv;

    /* Init libass */
    ret = init(ctx);
    if (ret < 0)
        return ret;
    ass->track = ass_new_track(ass->library);
    if (!ass->track) {
        av_log(ctx, AV_LOG_ERROR, "Could not create a libass track\n");
        return AVERROR(EINVAL);
    }

    /* Open subtitles file */
    ret = avformat_open_input(&fmt, ass->filename, NULL, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Unable to open %s\n", ass->filename);
        goto end;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0)
        goto end;

    /* Locate subtitles stream */
    if (ass->stream_index < 0)
        ret = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    else {
        ret = -1;
        if (ass->stream_index < fmt->nb_streams) {
            for (j = 0; j < fmt->nb_streams; j++) {
                if (fmt->streams[j]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    if (ass->stream_index == k) {
                        ret = j;
                        break;
                    }
                    k++;
                }
            }
        }
    }

    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Unable to locate subtitle stream in %s\n",
               ass->filename);
        goto end;
    }
    sid = ret;
    st = fmt->streams[sid];

    /* Load attached fonts */
    for (j = 0; j < fmt->nb_streams; j++) {
        AVStream *st = fmt->streams[j];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT &&
            attachment_is_font(st)) {
            const AVDictionaryEntry *tag = NULL;
            tag = av_dict_get(st->metadata, "filename", NULL,
                              AV_DICT_MATCH_CASE);

            if (tag) {
                av_log(ctx, AV_LOG_DEBUG, "Loading attached font: %s\n",
                       tag->value);
                ass_add_font(ass->library, tag->value,
                             st->codecpar->extradata,
                             st->codecpar->extradata_size);
            } else {
                av_log(ctx, AV_LOG_WARNING,
                       "Font attachment has no filename, ignored.\n");
            }
        }
    }

    /* Initialize fonts */
    ass_set_fonts(ass->renderer, NULL, NULL, 1, NULL, 1);

    /* Open decoder */
    dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        av_log(ctx, AV_LOG_ERROR, "Failed to find subtitle codec %s\n",
               avcodec_get_name(st->codecpar->codec_id));
        ret = AVERROR_DECODER_NOT_FOUND;
        goto end;
    }
    dec_desc = avcodec_descriptor_get(st->codecpar->codec_id);
    if (dec_desc && !(dec_desc->props & AV_CODEC_PROP_TEXT_SUB)) {
        av_log(ctx, AV_LOG_ERROR,
               "Only text based subtitles are currently supported\n");
        ret = AVERROR_PATCHWELCOME;
        goto end;
    }
    if (ass->charenc)
        av_dict_set(&codec_opts, "sub_charenc", ass->charenc, 0);

    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avcodec_parameters_to_context(dec_ctx, st->codecpar);
    if (ret < 0)
        goto end;

    /*
     * This is required by the decoding process in order to rescale the
     * timestamps: in the current API the decoded subtitles have their pts
     * expressed in AV_TIME_BASE, and thus the lavc internals need to know the
     * stream time base in order to achieve the rescaling.
     *
     * That API is old and needs to be reworked to match behaviour with A/V.
     */
    dec_ctx->pkt_timebase = st->time_base;

    ret = avcodec_open2(dec_ctx, NULL, &codec_opts);
    if (ret < 0)
        goto end;

#if FF_ASS_FEATURE_WRAP_UNICODE
    /* Don't overwrite wrap automatically for native ASS */
    if (ass->wrap_unicode == -1)
        ass->wrap_unicode = st->codecpar->codec_id != AV_CODEC_ID_ASS;
    if (ass->wrap_unicode) {
        ret = ass_track_set_feature(ass->track, ASS_FEATURE_WRAP_UNICODE, 1);
        if (ret < 0)
            av_log(ctx, AV_LOG_WARNING,
                   "libass wasn't built with ASS_FEATURE_WRAP_UNICODE support\n");
    }
#endif

    if (ass->force_style) {
        char **list = NULL;
        char *temp = NULL;
        char *ptr = av_strtok(ass->force_style, ",", &temp);
        int i = 0;
        while (ptr) {
            av_dynarray_add(&list, &i, ptr);
            if (!list) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            ptr = av_strtok(NULL, ",", &temp);
        }
        av_dynarray_add(&list, &i, NULL);
        if (!list) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        ass_set_style_overrides(ass->library, list);
        av_free(list);
    }
    /* Decode subtitles and push them into the renderer (libass) */
    if (dec_ctx->subtitle_header)
        ass_process_codec_private(ass->track,
                                  dec_ctx->subtitle_header,
                                  dec_ctx->subtitle_header_size);
    while (av_read_frame(fmt, &pkt) >= 0) {
        int i, got_subtitle;
        AVSubtitle sub = {0};

        if (pkt.stream_index == sid) {
            ret = avcodec_decode_subtitle2(dec_ctx, &sub, &got_subtitle, &pkt);
            if (ret < 0) {
                av_log(ctx, AV_LOG_WARNING, "Error decoding: %s (ignored)\n",
                       av_err2str(ret));
            } else if (got_subtitle) {
                const int64_t start_time = av_rescale_q(sub.pts, AV_TIME_BASE_Q, av_make_q(1, 1000));
                const int64_t duration   = sub.end_display_time;
                for (i = 0; i < sub.num_rects; i++) {
                    char *ass_line = sub.rects[i]->ass;
                    if (!ass_line)
                        break;
                    ass_process_chunk(ass->track, ass_line, strlen(ass_line),
                                      start_time, duration);
                }
            }
        }
        av_packet_unref(&pkt);
        avsubtitle_free(&sub);
    }

end:
    av_dict_free(&codec_opts);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt);
    return ret;
}

const FFFilter ff_vf_subtitles = {
    .p.name        = "subtitles",
    .p.description = NULL_IF_CONFIG_SMALL("Render text subtitles onto input video using the libass library."),
    .p.priv_class  = &subtitles_class,
    .priv_size     = sizeof(AssContext),
    .init          = init_subtitles,
    .uninit        = uninit,
    FILTER_INPUTS(ass_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
};
#endif
