/*
 * DVD subtitle decoding
 * Copyright (c) 2005 Fabrice Bellard
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

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "dvdsub.h"
#include "get_bits.h"

#include "libavutil/attributes.h"
#include "libavutil/colorspace.h"
#include "libavutil/file_open.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/bswap.h"

typedef struct DVDSubContext
{
  AVClass *class;
  uint32_t palette[16];
  char    *palette_str;
  char    *ifo_str;
  int      has_palette;
  uint8_t  colormap[4];
  uint8_t  alpha[256];
  uint8_t  buf[0x10000];
  int      buf_size;
  int      forced_subs_only;
  uint8_t  used_color[256];
} DVDSubContext;

static void yuv_a_to_rgba(const uint8_t *ycbcr, const uint8_t *alpha, uint32_t *rgba, int num_values)
{
    const uint8_t *cm = ff_crop_tab + MAX_NEG_CROP;
    uint8_t r, g, b;
    int i, y, cb, cr;
    int r_add, g_add, b_add;

    for (i = num_values; i > 0; i--) {
        y = *ycbcr++;
        cr = *ycbcr++;
        cb = *ycbcr++;
        YUV_TO_RGB1_CCIR(cb, cr);
        YUV_TO_RGB2_CCIR(r, g, b, y);
        *rgba++ = ((unsigned)*alpha++ << 24) | (r << 16) | (g << 8) | b;
    }
}

static int decode_run_2bit(GetBitContext *gb, int *color)
{
    unsigned int v, t;

    v = 0;
    for (t = 1; v < t && t <= 0x40; t <<= 2)
        v = (v << 4) | get_bits(gb, 4);
    *color = v & 3;
    if (v < 4) { /* Code for fill rest of line */
        return INT_MAX;
    }
    return v >> 2;
}

static int decode_run_8bit(GetBitContext *gb, int *color)
{
    int len;
    int has_run = get_bits1(gb);
    *color = get_bits(gb, 2 + 6*get_bits1(gb));
    if (has_run) {
        if (get_bits1(gb)) {
            len = get_bits(gb, 7);
            if (len == 0)
                len = INT_MAX;
            else
                len += 9;
        } else
            len = get_bits(gb, 3) + 2;
    } else
        len = 1;
    return len;
}

static int decode_rle(uint8_t *bitmap, int linesize, int w, int h, uint8_t used_color[256],
                      const uint8_t *buf, int start, int buf_size, int is_8bit)
{
    GetBitContext gb;
    int bit_len;
    int x, y, len, color;
    uint8_t *d;

    if (start >= buf_size)
        return -1;

    if (w <= 0 || h <= 0)
        return -1;

    bit_len = (buf_size - start) * 8;
    init_get_bits(&gb, buf + start, bit_len);

    x = 0;
    y = 0;
    d = bitmap;
    for(;;) {
        if (get_bits_count(&gb) > bit_len)
            return -1;
        if (is_8bit)
            len = decode_run_8bit(&gb, &color);
        else
            len = decode_run_2bit(&gb, &color);
        if (len != INT_MAX && len > w - x)
            return AVERROR_INVALIDDATA;
        len = FFMIN(len, w - x);
        memset(d + x, color, len);
        used_color[color] = 1;
        x += len;
        if (x >= w) {
            y++;
            if (y >= h)
                break;
            d += linesize;
            x = 0;
            /* byte align */
            align_get_bits(&gb);
        }
    }
    return 0;
}

static void guess_palette(DVDSubContext* ctx,
                          uint32_t *rgba_palette,
                          uint32_t subtitle_color)
{
    static const uint8_t level_map[4][4] = {
        // this configuration (full range, lowest to highest) in tests
        // seemed most common, so assume this
        {0xff},
        {0x00, 0xff},
        {0x00, 0x80, 0xff},
        {0x00, 0x55, 0xaa, 0xff},
    };
    uint8_t color_used[16] = { 0 };
    int nb_opaque_colors, i, level, j, r, g, b;
    uint8_t *colormap = ctx->colormap, *alpha = ctx->alpha;

    if(ctx->has_palette) {
        for(i = 0; i < 4; i++)
            rgba_palette[i] = (ctx->palette[colormap[i]] & 0x00ffffff)
                              | ((alpha[i] * 17U) << 24);
        return;
    }

    for(i = 0; i < 4; i++)
        rgba_palette[i] = 0;

    nb_opaque_colors = 0;
    for(i = 0; i < 4; i++) {
        if (alpha[i] != 0 && !color_used[colormap[i]]) {
            color_used[colormap[i]] = 1;
            nb_opaque_colors++;
        }
    }

    if (nb_opaque_colors == 0)
        return;

    j = 0;
    memset(color_used, 0, 16);
    for(i = 0; i < 4; i++) {
        if (alpha[i] != 0) {
            if (!color_used[colormap[i]])  {
                level = level_map[nb_opaque_colors - 1][j];
                r = (((subtitle_color >> 16) & 0xff) * level) >> 8;
                g = (((subtitle_color >> 8) & 0xff) * level) >> 8;
                b = (((subtitle_color >> 0) & 0xff) * level) >> 8;
                rgba_palette[i] = b | (g << 8) | (r << 16) | ((alpha[i] * 17U) << 24);
                color_used[colormap[i]] = (i + 1);
                j++;
            } else {
                rgba_palette[i] = (rgba_palette[color_used[colormap[i]] - 1] & 0x00ffffff) |
                                    ((alpha[i] * 17U) << 24);
            }
        }
    }
}

static void reset_rects(AVSubtitle *sub_header)
{
    int i;

    if (sub_header->rects) {
        for (i = 0; i < sub_header->num_rects; i++) {
            av_freep(&sub_header->rects[i]->data[0]);
            av_freep(&sub_header->rects[i]->data[1]);
            av_freep(&sub_header->rects[i]);
        }
        av_freep(&sub_header->rects);
        sub_header->num_rects = 0;
    }
}

#define READ_OFFSET(a) (big_offsets ? AV_RB32(a) : AV_RB16(a))

static int decode_dvd_subtitles(void *logctx, DVDSubContext *ctx,
                                AVSubtitle *sub_header, const uint8_t *buf, int buf_size)
{
    int cmd_pos, pos, cmd, x1, y1, x2, y2, next_cmd_pos;
    int big_offsets, offset_size, is_8bit = 0;
    const uint8_t *yuv_palette = NULL;
    uint8_t *colormap = ctx->colormap, *alpha = ctx->alpha;
    int date;
    int i;
    int is_menu = 0;
    uint32_t size;
    int64_t offset1, offset2;

    if (buf_size < 10)
        return -1;

    if (AV_RB16(buf) == 0) {   /* HD subpicture with 4-byte offsets */
        big_offsets = 1;
        offset_size = 4;
        cmd_pos = 6;
    } else {
        big_offsets = 0;
        offset_size = 2;
        cmd_pos = 2;
    }

    size = READ_OFFSET(buf + (big_offsets ? 2 : 0));
    cmd_pos = READ_OFFSET(buf + cmd_pos);

    if (cmd_pos < 0 || cmd_pos > buf_size - 2 - offset_size) {
        if (cmd_pos > size) {
            av_log(logctx, AV_LOG_ERROR, "Discarding invalid packet\n");
            return 0;
        }
        return AVERROR(EAGAIN);
    }

    while (cmd_pos > 0 && cmd_pos < buf_size - 2 - offset_size) {
        date = AV_RB16(buf + cmd_pos);
        next_cmd_pos = READ_OFFSET(buf + cmd_pos + 2);
        ff_dlog(logctx, "cmd_pos=0x%04x next=0x%04x date=%d\n",
                cmd_pos, next_cmd_pos, date);
        pos = cmd_pos + 2 + offset_size;
        offset1 = -1;
        offset2 = -1;
        x1 = y1 = x2 = y2 = 0;
        while (pos < buf_size) {
            cmd = buf[pos++];
            ff_dlog(logctx, "cmd=%02x\n", cmd);
            switch(cmd) {
            case 0x00:
                /* menu subpicture */
                is_menu = 1;
                break;
            case 0x01:
                /* set start date */
                sub_header->start_display_time = (date << 10) / 90;
                break;
            case 0x02:
                /* set end date */
                sub_header->end_display_time = (date << 10) / 90;
                break;
            case 0x03:
                /* set colormap */
                if ((buf_size - pos) < 2)
                    goto fail;
                colormap[3] = buf[pos] >> 4;
                colormap[2] = buf[pos] & 0x0f;
                colormap[1] = buf[pos + 1] >> 4;
                colormap[0] = buf[pos + 1] & 0x0f;
                pos += 2;
                break;
            case 0x04:
                /* set alpha */
                if ((buf_size - pos) < 2)
                    goto fail;
                alpha[3] = buf[pos] >> 4;
                alpha[2] = buf[pos] & 0x0f;
                alpha[1] = buf[pos + 1] >> 4;
                alpha[0] = buf[pos + 1] & 0x0f;
                pos += 2;
                ff_dlog(logctx, "alpha=%x%x%x%x\n", alpha[0],alpha[1],alpha[2],alpha[3]);
                break;
            case 0x05:
            case 0x85:
                if ((buf_size - pos) < 6)
                    goto fail;
                x1 = (buf[pos] << 4) | (buf[pos + 1] >> 4);
                x2 = ((buf[pos + 1] & 0x0f) << 8) | buf[pos + 2];
                y1 = (buf[pos + 3] << 4) | (buf[pos + 4] >> 4);
                y2 = ((buf[pos + 4] & 0x0f) << 8) | buf[pos + 5];
                if (cmd & 0x80)
                    is_8bit = 1;
                ff_dlog(logctx, "x1=%d x2=%d y1=%d y2=%d\n", x1, x2, y1, y2);
                pos += 6;
                break;
            case 0x06:
                if ((buf_size - pos) < 4)
                    goto fail;
                offset1 = AV_RB16(buf + pos);
                offset2 = AV_RB16(buf + pos + 2);
                ff_dlog(logctx, "offset1=0x%04"PRIx64" offset2=0x%04"PRIx64"\n", offset1, offset2);
                pos += 4;
                break;
            case 0x86:
                if ((buf_size - pos) < 8)
                    goto fail;
                offset1 = AV_RB32(buf + pos);
                offset2 = AV_RB32(buf + pos + 4);
                ff_dlog(logctx, "offset1=0x%04"PRIx64" offset2=0x%04"PRIx64"\n", offset1, offset2);
                pos += 8;
                break;

            case 0x83:
                /* HD set palette */
                if ((buf_size - pos) < 768)
                    goto fail;
                yuv_palette = buf + pos;
                pos += 768;
                break;
            case 0x84:
                /* HD set contrast (alpha) */
                if ((buf_size - pos) < 256)
                    goto fail;
                for (i = 0; i < 256; i++)
                    alpha[i] = 0xFF - buf[pos+i];
                pos += 256;
                break;

            case 0xff:
                goto the_end;
            default:
                ff_dlog(logctx, "unrecognised subpicture command 0x%x\n", cmd);
                goto the_end;
            }
        }
    the_end:
        if (offset1 >= buf_size || offset2 >= buf_size)
            goto fail;

        if (offset1 >= 0 && offset2 >= 0) {
            int w, h;
            uint8_t *bitmap;

            /* decode the bitmap */
            w = x2 - x1 + 1;
            if (w < 0)
                w = 0;
            h = y2 - y1 + 1;
            if (h < 0)
                h = 0;
            if (w > 0 && h > 1) {
                reset_rects(sub_header);
                memset(ctx->used_color, 0, sizeof(ctx->used_color));
                sub_header->rects = av_mallocz(sizeof(*sub_header->rects));
                if (!sub_header->rects)
                    goto fail;
                sub_header->rects[0] = av_mallocz(sizeof(AVSubtitleRect));
                if (!sub_header->rects[0])
                    goto fail;
                sub_header->num_rects = 1;
                bitmap = sub_header->rects[0]->data[0] = av_malloc(w * h);
                if (!bitmap)
                    goto fail;
                if (decode_rle(bitmap, w * 2, w, (h + 1) / 2, ctx->used_color,
                               buf, offset1, buf_size, is_8bit) < 0)
                    goto fail;
                if (decode_rle(bitmap + w, w * 2, w, h / 2, ctx->used_color,
                               buf, offset2, buf_size, is_8bit) < 0)
                    goto fail;
                sub_header->rects[0]->data[1] = av_mallocz(AVPALETTE_SIZE);
                if (!sub_header->rects[0]->data[1])
                    goto fail;
                if (is_8bit) {
                    if (!yuv_palette)
                        goto fail;
                    sub_header->rects[0]->nb_colors = 256;
                    yuv_a_to_rgba(yuv_palette, alpha,
                                  (uint32_t *)sub_header->rects[0]->data[1],
                                  256);
                } else {
                    sub_header->rects[0]->nb_colors = 4;
                    guess_palette(ctx, (uint32_t*)sub_header->rects[0]->data[1],
                                  0xffffff);
                }
                sub_header->rects[0]->x = x1;
                sub_header->rects[0]->y = y1;
                sub_header->rects[0]->w = w;
                sub_header->rects[0]->h = h;
                sub_header->rects[0]->type = SUBTITLE_BITMAP;
                sub_header->rects[0]->linesize[0] = w;
                sub_header->rects[0]->flags = is_menu ? AV_SUBTITLE_FLAG_FORCED : 0;
            }
        }
        if (next_cmd_pos < cmd_pos) {
            av_log(logctx, AV_LOG_ERROR, "Invalid command offset\n");
            break;
        }
        if (next_cmd_pos == cmd_pos)
            break;
        cmd_pos = next_cmd_pos;
    }
    if (sub_header->num_rects > 0)
        return is_menu;
 fail:
    reset_rects(sub_header);
    return -1;
}

static int is_transp(const uint8_t *buf, int pitch, int n,
                     const uint8_t *transp_color)
{
    int i;
    for(i = 0; i < n; i++) {
        if (!transp_color[*buf])
            return 0;
        buf += pitch;
    }
    return 1;
}

/* return 0 if empty rectangle, 1 if non empty */
static int find_smallest_bounding_rectangle(DVDSubContext *ctx, AVSubtitle *s)
{
    uint8_t transp_color[256] = { 0 };
    int y1, y2, x1, x2, y, w, h, i;
    uint8_t *bitmap;
    int transparent = 1;

    if (s->num_rects == 0 || !s->rects || s->rects[0]->w <= 0 || s->rects[0]->h <= 0)
        return 0;

    for(i = 0; i < s->rects[0]->nb_colors; i++) {
        if ((((uint32_t *)s->rects[0]->data[1])[i] >> 24) == 0) {
            transp_color[i] = 1;
        } else if (ctx->used_color[i])
            transparent = 0;
    }
    if (transparent)
        return 0;
    y1 = 0;
    while (y1 < s->rects[0]->h && is_transp(s->rects[0]->data[0] + y1 * s->rects[0]->linesize[0],
                                  1, s->rects[0]->w, transp_color))
        y1++;
    if (y1 == s->rects[0]->h) {
        av_freep(&s->rects[0]->data[0]);
        s->rects[0]->w = s->rects[0]->h = 0;
        return 0;
    }

    y2 = s->rects[0]->h - 1;
    while (y2 > 0 && is_transp(s->rects[0]->data[0] + y2 * s->rects[0]->linesize[0], 1,
                               s->rects[0]->w, transp_color))
        y2--;
    x1 = 0;
    while (x1 < (s->rects[0]->w - 1) && is_transp(s->rects[0]->data[0] + x1, s->rects[0]->linesize[0],
                                        s->rects[0]->h, transp_color))
        x1++;
    x2 = s->rects[0]->w - 1;
    while (x2 > 0 && is_transp(s->rects[0]->data[0] + x2, s->rects[0]->linesize[0], s->rects[0]->h,
                                  transp_color))
        x2--;
    w = x2 - x1 + 1;
    h = y2 - y1 + 1;
    bitmap = av_malloc(w * h);
    if (!bitmap)
        return 1;
    for(y = 0; y < h; y++) {
        memcpy(bitmap + w * y, s->rects[0]->data[0] + x1 + (y1 + y) * s->rects[0]->linesize[0], w);
    }
    av_freep(&s->rects[0]->data[0]);
    s->rects[0]->data[0] = bitmap;
    s->rects[0]->linesize[0] = w;
    s->rects[0]->w = w;
    s->rects[0]->h = h;
    s->rects[0]->x += x1;
    s->rects[0]->y += y1;

    return 1;
}

static int append_to_cached_buf(AVCodecContext *avctx,
                                const uint8_t *buf, int buf_size)
{
    DVDSubContext *ctx = avctx->priv_data;

    av_assert0(buf_size >= 0 && ctx->buf_size <= sizeof(ctx->buf));
    if (buf_size >= sizeof(ctx->buf) - ctx->buf_size) {
        av_log(avctx, AV_LOG_WARNING, "Attempt to reconstruct "
               "too large SPU packets aborted.\n");
        ctx->buf_size = 0;
        return AVERROR_INVALIDDATA;
    }
    memcpy(ctx->buf + ctx->buf_size, buf, buf_size);
    ctx->buf_size += buf_size;
    return 0;
}

static int dvdsub_decode(AVCodecContext *avctx, AVSubtitle *sub,
                         int *data_size, const AVPacket *avpkt)
{
    DVDSubContext *ctx = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    int appended = 0;
    int is_menu;

    if (ctx->buf_size) {
        int ret = append_to_cached_buf(avctx, buf, buf_size);
        if (ret < 0) {
            *data_size = 0;
            return ret;
        }
        buf = ctx->buf;
        buf_size = ctx->buf_size;
        appended = 1;
    }

    is_menu = decode_dvd_subtitles(avctx, ctx, sub, buf, buf_size);
    if (is_menu == AVERROR(EAGAIN)) {
        *data_size = 0;
        return appended ? 0 : append_to_cached_buf(avctx, buf, buf_size);
    }

    if (is_menu < 0) {
        ctx->buf_size = 0;
    no_subtitle:
        reset_rects(sub);
        *data_size = 0;

        return buf_size;
    }
    if (!is_menu && find_smallest_bounding_rectangle(ctx, sub) == 0)
        goto no_subtitle;

    if (ctx->forced_subs_only && !(sub->rects[0]->flags & AV_SUBTITLE_FLAG_FORCED))
        goto no_subtitle;

    ctx->buf_size = 0;
    *data_size = 1;
    return buf_size;
}

static int parse_ifo_palette(void *logctx, DVDSubContext *ctx, char *p)
{
    FILE *ifo;
    char ifostr[12];
    uint32_t sp_pgci, pgci, off_pgc, pgc;
    uint8_t r, g, b, yuv[65], *buf;
    int i, y, cb, cr, r_add, g_add, b_add;
    int ret = 0;
    const uint8_t *cm = ff_crop_tab + MAX_NEG_CROP;

    ctx->has_palette = 0;
    if ((ifo = avpriv_fopen_utf8(p, "r")) == NULL) {
        av_log(logctx, AV_LOG_WARNING, "Unable to open IFO file \"%s\": %s\n", p, av_err2str(AVERROR(errno)));
        return AVERROR_EOF;
    }
    if (fread(ifostr, 12, 1, ifo) != 1 || memcmp(ifostr, "DVDVIDEO-VTS", 12)) {
        av_log(logctx, AV_LOG_WARNING, "\"%s\" is not a proper IFO file\n", p);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (fseek(ifo, 0xCC, SEEK_SET) == -1) {
        ret = AVERROR(errno);
        goto end;
    }
    if (fread(&sp_pgci, 4, 1, ifo) == 1) {
        pgci = av_be2ne32(sp_pgci) * 2048;
        if (fseek(ifo, pgci + 0x0C, SEEK_SET) == -1) {
            ret = AVERROR(errno);
            goto end;
        }
        if (fread(&off_pgc, 4, 1, ifo) == 1) {
            pgc = pgci + av_be2ne32(off_pgc);
            if (fseek(ifo, pgc + 0xA4, SEEK_SET) == -1) {
                ret = AVERROR(errno);
                goto end;
            }
            if (fread(yuv, 64, 1, ifo) == 1) {
                buf = yuv;
                for(i=0; i<16; i++) {
                    y  = *++buf;
                    cr = *++buf;
                    cb = *++buf;
                    YUV_TO_RGB1_CCIR(cb, cr);
                    YUV_TO_RGB2_CCIR(r, g, b, y);
                    ctx->palette[i] = (r << 16) + (g << 8) + b;
                    buf++;
                }
                ctx->has_palette = 1;
            }
        }
    }
    if (ctx->has_palette == 0) {
        av_log(logctx, AV_LOG_WARNING, "Failed to read palette from IFO file \"%s\"\n", p);
        ret = AVERROR_INVALIDDATA;
    }
end:
    fclose(ifo);
    return ret;
}

static int dvdsub_parse_extradata(AVCodecContext *avctx)
{
    DVDSubContext *ctx = (DVDSubContext*) avctx->priv_data;
    char *dataorig, *data;
    int ret = 1;

    if (!avctx->extradata || !avctx->extradata_size)
        return 1;

    dataorig = data = av_malloc(avctx->extradata_size+1);
    if (!data)
        return AVERROR(ENOMEM);
    memcpy(data, avctx->extradata, avctx->extradata_size);
    data[avctx->extradata_size] = '\0';

    for(;;) {
        int pos = strcspn(data, "\n\r");
        if (pos==0 && *data==0)
            break;

        if (strncmp("palette:", data, 8) == 0) {
            ctx->has_palette = 1;
            ff_dvdsub_parse_palette(ctx->palette, data + 8);
        } else if (strncmp("size:", data, 5) == 0) {
            int w, h;
            if (sscanf(data + 5, "%dx%d", &w, &h) == 2) {
               ret = ff_set_dimensions(avctx, w, h);
               if (ret < 0)
                   goto fail;
            }
        }

        data += pos;
        data += strspn(data, "\n\r");
    }

fail:
    av_free(dataorig);
    return ret;
}

static av_cold int dvdsub_init(AVCodecContext *avctx)
{
    DVDSubContext *ctx = avctx->priv_data;
    int ret;

    if ((ret = dvdsub_parse_extradata(avctx)) < 0)
        return ret;

    if (ctx->ifo_str)
        parse_ifo_palette(avctx, ctx, ctx->ifo_str);
    if (ctx->palette_str) {
        ctx->has_palette = 1;
        ff_dvdsub_parse_palette(ctx->palette, ctx->palette_str);
    }
    if (ctx->has_palette) {
        int i;
        av_log(avctx, AV_LOG_DEBUG, "palette:");
        for(i=0;i<16;i++)
            av_log(avctx, AV_LOG_DEBUG, " 0x%06"PRIx32, ctx->palette[i]);
        av_log(avctx, AV_LOG_DEBUG, "\n");
    }

    return 1;
}

static void dvdsub_flush(AVCodecContext *avctx)
{
    DVDSubContext *ctx = avctx->priv_data;
    ctx->buf_size = 0;
}

#define OFFSET(field) offsetof(DVDSubContext, field)
#define SD AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "palette", "set the global palette", OFFSET(palette_str), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, SD },
    { "ifo_palette", "obtain the global palette from .IFO file", OFFSET(ifo_str), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, SD },
    { "forced_subs_only", "Only show forced subtitles", OFFSET(forced_subs_only), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, SD},
    { NULL }
};
static const AVClass dvdsub_class = {
    .class_name = "dvdsubdec",
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_dvdsub_decoder = {
    .p.name         = "dvdsub",
    CODEC_LONG_NAME("DVD subtitles"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_DVD_SUBTITLE,
    .priv_data_size = sizeof(DVDSubContext),
    .init           = dvdsub_init,
    FF_CODEC_DECODE_SUB_CB(dvdsub_decode),
    .flush          = dvdsub_flush,
    .p.priv_class   = &dvdsub_class,
};
