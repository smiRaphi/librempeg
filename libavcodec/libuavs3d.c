/*
 * AVS3-P2/IEEE1857.10 video decoder (using the uavs3d library)
 * Copyright (c) 2020 Zhenyu Wang <wangzhenyu@pkusz.edu.cn>
 *                    Bingjie Han <hanbj@pkusz.edu.cn>
 *                    Huiwen Ren  <hwrenx@gmail.com>
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

#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/cpu.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "avs3.h"
#include "codec_internal.h"
#include "decode.h"
#include "uavs3d.h"

typedef struct uavs3d_context {
    AVCodecContext  *avctx;
    void            *dec_handle;
    int              frame_threads;
    int              got_seqhdr;
    uavs3d_io_frm_t  dec_frame;
} uavs3d_context;

#define UAVS3D_CHECK_START_CODE(data_ptr, PIC_START_CODE) \
        (AV_RL32(data_ptr) != (PIC_START_CODE << 24) + AVS3_NAL_START_CODE)
static int uavs3d_find_next_start_code(const unsigned char *bs_data, int bs_len, int *left)
{
    const unsigned char *data_ptr = bs_data + 4;
    int count = bs_len - 4;

    while (count >= 4 &&
           UAVS3D_CHECK_START_CODE(data_ptr, AVS3_INTER_PIC_START_CODE) &&
           UAVS3D_CHECK_START_CODE(data_ptr, AVS3_INTRA_PIC_START_CODE) &&
           UAVS3D_CHECK_START_CODE(data_ptr, AVS3_SEQ_START_CODE) &&
           UAVS3D_CHECK_START_CODE(data_ptr, AVS3_FIRST_SLICE_START_CODE) &&
           UAVS3D_CHECK_START_CODE(data_ptr, AVS3_SEQ_END_CODE)) {
        data_ptr++;
        count--;
    }

    if (count >= 4) {
        *left = count;
        return 1;
    }

    return 0;
}

static void uavs3d_output_callback(uavs3d_io_frm_t *dec_frame) {
    uavs3d_io_frm_t frm_out;
    AVFrame *frm = (AVFrame *)dec_frame->priv;
    int i;

    if (!frm || !frm->data[0]) {
        dec_frame->got_pic = 0;
        av_log(NULL, AV_LOG_ERROR, "Invalid AVFrame in uavs3d output.\n");
        return;
    }

    frm->pts       = dec_frame->pts;
    frm->pkt_dts   = dec_frame->dts;

    if (dec_frame->type < 0 || dec_frame->type >= FF_ARRAY_ELEMS(ff_avs3_image_type)) {
        av_log(NULL, AV_LOG_WARNING, "Error frame type in uavs3d: %d.\n", dec_frame->type);
    } else {
        frm->pict_type = ff_avs3_image_type[dec_frame->type];
        if (frm->pict_type == AV_PICTURE_TYPE_I)
            frm->flags |= AV_FRAME_FLAG_KEY;
        else
            frm->flags &= ~AV_FRAME_FLAG_KEY;
    }

    for (i = 0; i < 3; i++) {
        frm_out.width [i] = dec_frame->width[i];
        frm_out.height[i] = dec_frame->height[i];
        frm_out.stride[i] = frm->linesize[i];
        frm_out.buffer[i] = frm->data[i];
    }

    uavs3d_img_cpy_cvt(&frm_out, dec_frame, dec_frame->bit_depth);
}

#define UAVS3D_CHECK_INVALID_RANGE(v, l, r) ((v)<(l)||(v)>(r))

static int libuavs3d_on_seq_header(AVCodecContext *avctx)
{
    uavs3d_context *h = avctx->priv_data;
    uavs3d_io_frm_t *frm_dec = &h->dec_frame;
    struct uavs3d_com_seqh_t *seqh = frm_dec->seqhdr;
    int ret;

    if (UAVS3D_CHECK_INVALID_RANGE(seqh->frame_rate_code, 0, 15)) {
        av_log(avctx, AV_LOG_ERROR, "Invalid frame rate code: %d.\n", seqh->frame_rate_code);
        seqh->frame_rate_code = 3; // default 25 fps
    } else {
        avctx->framerate.num = ff_avs3_frame_rate_tab[seqh->frame_rate_code].num;
        avctx->framerate.den = ff_avs3_frame_rate_tab[seqh->frame_rate_code].den;
    }
    avctx->has_b_frames = seqh->output_reorder_delay;
    avctx->pix_fmt = seqh->bit_depth_internal == 8 ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_YUV420P10;
    ret = ff_set_dimensions(avctx, seqh->horizontal_size, seqh->vertical_size);
    if (ret < 0)
        return ret;
    h->got_seqhdr = 1;

    if (seqh->colour_description) {
        if (UAVS3D_CHECK_INVALID_RANGE(seqh->colour_primaries, 0, 9) ||
            UAVS3D_CHECK_INVALID_RANGE(seqh->transfer_characteristics, 0, 14) ||
            UAVS3D_CHECK_INVALID_RANGE(seqh->matrix_coefficients, 0, 11)) {
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid colour description: primaries: %d"
                   "transfer characteristics: %d"
                   "matrix coefficients: %d.\n",
                   seqh->colour_primaries,
                   seqh->transfer_characteristics,
                   seqh->matrix_coefficients);
        } else {
            avctx->color_primaries = ff_avs3_color_primaries_tab[seqh->colour_primaries];
            avctx->color_trc       = ff_avs3_color_transfer_tab [seqh->transfer_characteristics];
            avctx->colorspace      = ff_avs3_color_matrix_tab   [seqh->matrix_coefficients];
        }
    }

    return 0;
}

static int libuavs3d_decode_extradata(AVCodecContext *avctx)
{
    uavs3d_context *h = avctx->priv_data;
    uint8_t *header = avctx->extradata;
    int header_size = avctx->extradata_size;
    uavs3d_io_frm_t *frm_dec = &h->dec_frame;

    if (avctx->extradata_size < 4) {
        av_log(avctx, AV_LOG_WARNING, "Invalid extradata size %d\n",
               avctx->extradata_size);
        return 0;
    }

    if (header[0] == 1) {
        // Skip configurationVersion and sequence_header_length
        header += 3;
        // Also remove library_dependency_idc at the end
        header_size -= 4;
    }

    frm_dec->nal_type = 0;
    frm_dec->pkt_pos = 0;
    frm_dec->pkt_size = header_size;
    frm_dec->bs = header;
    frm_dec->bs_len = header_size;
    frm_dec->pts = 0;
    frm_dec->dts = 0;
    uavs3d_decode(h->dec_handle, frm_dec);
    if (frm_dec->nal_type == NAL_SEQ_HEADER) {
        int ret = libuavs3d_on_seq_header(avctx);
        if (ret < 0)
            av_log(avctx, AV_LOG_WARNING,
                   "Process sequence header failed, %s\n", av_err2str(ret));
    } else {
        av_log(avctx, AV_LOG_WARNING,
               "Missing sequence header in extradata\n");
    }

    return 0;
}

static av_cold int libuavs3d_init(AVCodecContext *avctx)
{
    uavs3d_context *h = avctx->priv_data;
    uavs3d_cfg_t cdsc;

    cdsc.frm_threads = avctx->thread_count > 0 ? avctx->thread_count : av_cpu_count();
    cdsc.check_md5 = 0;
    h->dec_handle = uavs3d_create(&cdsc, uavs3d_output_callback, NULL);
    h->got_seqhdr = 0;

    if (!h->dec_handle) {
        return AVERROR(ENOMEM);
    }

    if (avctx->extradata)
        return libuavs3d_decode_extradata(avctx);

    return 0;
}

static av_cold int libuavs3d_end(AVCodecContext *avctx)
{
    uavs3d_context *h = avctx->priv_data;

    if (h->dec_handle) {
        uavs3d_flush(h->dec_handle, NULL);
        uavs3d_delete(h->dec_handle);
        h->dec_handle = NULL;
    }
    h->got_seqhdr = 0;

    return 0;
}

static void libuavs3d_flush(AVCodecContext * avctx)
{
    uavs3d_context *h = avctx->priv_data;

    if (h->dec_handle) {
        uavs3d_reset(h->dec_handle);
    }
}

static int libuavs3d_decode_frame(AVCodecContext *avctx, AVFrame *frm,
                                  int *got_frame, AVPacket *avpkt)
{
    uavs3d_context *h = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    const uint8_t *buf_end;
    const uint8_t *buf_ptr = buf;
    int left_bytes;
    int ret, finish = 0;

    *got_frame = 0;
    frm->pts = -1;
    frm->pict_type = AV_PICTURE_TYPE_NONE;

    if (!buf_size) {
        if (h->got_seqhdr) {
            if (!frm->data[0] && (ret = ff_get_buffer(avctx, frm, 0)) < 0) {
                return ret;
            }
            h->dec_frame.priv = frm;   // AVFrame
        }
        do {
            ret = uavs3d_flush(h->dec_handle, &h->dec_frame);
        } while (ret > 0 && !h->dec_frame.got_pic);
    } else {
        uavs3d_io_frm_t *frm_dec = &h->dec_frame;

        buf_end = buf + buf_size;

        while (!finish) {
            int bs_len;

            if (h->got_seqhdr) {
                if (!frm->data[0] && (ret = ff_get_buffer(avctx, frm, 0)) < 0) {
                    return ret;
                }
                h->dec_frame.priv = frm;   // AVFrame
            }

            if (uavs3d_find_next_start_code(buf_ptr, buf_end - buf_ptr, &left_bytes)) {
                bs_len = buf_end - buf_ptr - left_bytes;
            } else {
                bs_len = buf_end - buf_ptr;
                finish = 1;
            }
            frm_dec->bs = (unsigned char *)buf_ptr;
            frm_dec->bs_len = bs_len;
            frm_dec->pts = avpkt->pts;
            frm_dec->dts = avpkt->dts;
            uavs3d_decode(h->dec_handle, frm_dec);
            buf_ptr += bs_len;

            if (frm_dec->nal_type == NAL_SEQ_HEADER) {
                ret = libuavs3d_on_seq_header(avctx);
                if (ret < 0)
                    return ret;
            }
            if (frm_dec->got_pic) {
                break;
            }
        }
    }

    *got_frame = h->dec_frame.got_pic;

    if (!(*got_frame)) {
        av_frame_unref(frm);
    }

    return buf_ptr - buf;
}

const FFCodec ff_libuavs3d_decoder = {
    .p.name         = "libuavs3d",
    CODEC_LONG_NAME("libuavs3d AVS3-P2/IEEE1857.10"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_AVS3,
    .priv_data_size = sizeof(uavs3d_context),
    .init           = libuavs3d_init,
    .close          = libuavs3d_end,
    FF_CODEC_DECODE_CB(libuavs3d_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS,
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE |
                      FF_CODEC_CAP_AUTO_THREADS,
    .flush          = libuavs3d_flush,
    .p.wrapper_name = "libuavs3d",
};
