/*
 * Live smooth streaming fragmenter
 * Copyright (c) 2012 Martin Storsjo
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

#include "config.h"
#if HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "os_support.h"
#include "avc.h"
#include "url.h"

#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "libavutil/mathematics.h"
#include "libavutil/uuid.h"

typedef struct Fragment {
    int64_t start_time, duration;
    int n;
    int64_t start_pos, size;
    char file[1024];
    char infofile[1024];
} Fragment;

typedef struct OutputStream {
    AVFormatContext *ctx;
    URLContext *out;  // Current output stream where all output is written
    URLContext *out2; // Auxiliary output stream where all output is also written
    URLContext *tail_out; // The actual main output stream, if we're currently seeked back to write elsewhere
    int64_t tail_pos, cur_pos, cur_start_pos;
    int packets_written;
    const char *stream_type_tag;
    int nb_fragments, fragments_size, fragment_index;
    Fragment **fragments;

    const char *fourcc;
    char *private_str;
    int packet_size;
    int audio_tag;
    char dirname[1024];
    uint8_t iobuf[32768];
} OutputStream;

typedef struct SmoothStreamingContext {
    const AVClass *class;  /* Class for private options. */
    int window_size;
    int extra_window_size;
    int lookahead_count;
    int min_frag_duration;
    int remove_at_exit;
    OutputStream *streams;
    int has_video, has_audio;
    int nb_fragments;
} SmoothStreamingContext;

static int ism_write(void *opaque, const uint8_t *buf, int buf_size)
{
    OutputStream *os = opaque;
    if (os->out)
        ffurl_write(os->out, buf, buf_size);
    if (os->out2)
        ffurl_write(os->out2, buf, buf_size);
    os->cur_pos += buf_size;
    if (os->cur_pos >= os->tail_pos)
        os->tail_pos = os->cur_pos;
    return buf_size;
}

static int64_t ism_seek(void *opaque, int64_t offset, int whence)
{
    OutputStream *os = opaque;
    int i;
    if (whence != SEEK_SET)
        return AVERROR(ENOSYS);
    if (os->tail_out) {
        ffurl_closep(&os->out);
        ffurl_closep(&os->out2);
        os->out = os->tail_out;
        os->tail_out = NULL;
    }
    if (offset >= os->cur_start_pos) {
        if (os->out)
            ffurl_seek(os->out, offset - os->cur_start_pos, SEEK_SET);
        os->cur_pos = offset;
        return offset;
    }
    for (i = os->nb_fragments - 1; i >= 0; i--) {
        Fragment *frag = os->fragments[i];
        if (offset >= frag->start_pos && offset < frag->start_pos + frag->size) {
            int ret;
            AVDictionary *opts = NULL;
            os->tail_out = os->out;
            av_dict_set(&opts, "truncate", "0", 0);
            ret = ffurl_open_whitelist(&os->out, frag->file, AVIO_FLAG_WRITE,
                                       &os->ctx->interrupt_callback, &opts, os->ctx->protocol_whitelist, os->ctx->protocol_blacklist, NULL);
            av_dict_free(&opts);
            if (ret < 0) {
                os->out = os->tail_out;
                os->tail_out = NULL;
                return ret;
            }
            av_dict_set(&opts, "truncate", "0", 0);
            ffurl_open_whitelist(&os->out2, frag->infofile, AVIO_FLAG_WRITE,
                                 &os->ctx->interrupt_callback, &opts, os->ctx->protocol_whitelist, os->ctx->protocol_blacklist, NULL);
            av_dict_free(&opts);
            ffurl_seek(os->out, offset - frag->start_pos, SEEK_SET);
            if (os->out2)
                ffurl_seek(os->out2, offset - frag->start_pos, SEEK_SET);
            os->cur_pos = offset;
            return offset;
        }
    }
    return AVERROR(EIO);
}

static void get_private_data(OutputStream *os)
{
    AVCodecParameters *par = os->ctx->streams[0]->codecpar;
    uint8_t *ptr = par->extradata;
    int size = par->extradata_size;
    int i;
    if (par->codec_id == AV_CODEC_ID_H264) {
        ff_avc_write_annexb_extradata(ptr, &ptr, &size);
        if (!ptr)
            ptr = par->extradata;
    }
    if (!ptr)
        return;
    os->private_str = av_mallocz(2*size + 1);
    if (!os->private_str)
        goto fail;
    for (i = 0; i < size; i++)
        snprintf(&os->private_str[2*i], 3, "%02x", ptr[i]);
fail:
    if (ptr != par->extradata)
        av_free(ptr);
}

static void ism_free(AVFormatContext *s)
{
    SmoothStreamingContext *c = s->priv_data;
    int i, j;
    if (!c->streams)
        return;
    for (i = 0; i < s->nb_streams; i++) {
        OutputStream *os = &c->streams[i];
        ffurl_closep(&os->out);
        ffurl_closep(&os->out2);
        ffurl_closep(&os->tail_out);
        if (os->ctx && os->ctx->pb)
            avio_context_free(&os->ctx->pb);
        avformat_free_context(os->ctx);
        av_freep(&os->private_str);
        for (j = 0; j < os->nb_fragments; j++)
            av_freep(&os->fragments[j]);
        av_freep(&os->fragments);
    }
    av_freep(&c->streams);
}

static void output_chunk_list(OutputStream *os, AVIOContext *out, int final, int skip, int window_size)
{
    int removed = 0, i, start = 0;
    if (os->nb_fragments <= 0)
        return;
    if (os->fragments[0]->n > 0)
        removed = 1;
    if (final)
        skip = 0;
    if (window_size)
        start = FFMAX(os->nb_fragments - skip - window_size, 0);
    for (i = start; i < os->nb_fragments - skip; i++) {
        Fragment *frag = os->fragments[i];
        if (!final || removed)
            avio_printf(out, "<c t=\"%"PRIu64"\" d=\"%"PRIu64"\" />\n", frag->start_time, frag->duration);
        else
            avio_printf(out, "<c n=\"%d\" d=\"%"PRIu64"\" />\n", frag->n, frag->duration);
    }
}

static int write_manifest(AVFormatContext *s, int final)
{
    SmoothStreamingContext *c = s->priv_data;
    AVIOContext *out;
    char filename[1024], temp_filename[1024];
    int ret, i, video_chunks = 0, audio_chunks = 0, video_streams = 0, audio_streams = 0;
    int64_t duration = 0;

    snprintf(filename, sizeof(filename), "%s/Manifest", s->url);
    snprintf(temp_filename, sizeof(temp_filename), "%s/Manifest.tmp", s->url);
    ret = s->io_open(s, &out, temp_filename, AVIO_FLAG_WRITE, NULL);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Unable to open %s for writing\n", temp_filename);
        return ret;
    }
    avio_printf(out, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    for (i = 0; i < s->nb_streams; i++) {
        OutputStream *os = &c->streams[i];
        if (os->nb_fragments > 0) {
            Fragment *last = os->fragments[os->nb_fragments - 1];
            duration = last->start_time + last->duration;
        }
        if (s->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_chunks = os->nb_fragments;
            video_streams++;
        } else {
            audio_chunks = os->nb_fragments;
            audio_streams++;
        }
    }
    if (!final) {
        duration = 0;
        video_chunks = audio_chunks = 0;
    }
    if (c->window_size) {
        video_chunks = FFMIN(video_chunks, c->window_size);
        audio_chunks = FFMIN(audio_chunks, c->window_size);
    }
    avio_printf(out, "<SmoothStreamingMedia MajorVersion=\"2\" MinorVersion=\"0\" Duration=\"%"PRIu64"\"", duration);
    if (!final)
        avio_printf(out, " IsLive=\"true\" LookAheadFragmentCount=\"%d\" DVRWindowLength=\"0\"", c->lookahead_count);
    avio_printf(out, ">\n");
    if (c->has_video) {
        int last = -1, index = 0;
        avio_printf(out, "<StreamIndex Type=\"video\" QualityLevels=\"%d\" Chunks=\"%d\" Url=\"QualityLevels({bitrate})/Fragments(video={start time})\">\n", video_streams, video_chunks);
        for (i = 0; i < s->nb_streams; i++) {
            OutputStream *os = &c->streams[i];
            if (s->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                continue;
            last = i;
            avio_printf(out, "<QualityLevel Index=\"%d\" Bitrate=\"%"PRId64"\" FourCC=\"%s\" MaxWidth=\"%d\" MaxHeight=\"%d\" CodecPrivateData=\"%s\" />\n", index, s->streams[i]->codecpar->bit_rate, os->fourcc, s->streams[i]->codecpar->width, s->streams[i]->codecpar->height, os->private_str);
            index++;
        }
        output_chunk_list(&c->streams[last], out, final, c->lookahead_count, c->window_size);
        avio_printf(out, "</StreamIndex>\n");
    }
    if (c->has_audio) {
        int last = -1, index = 0;
        avio_printf(out, "<StreamIndex Type=\"audio\" QualityLevels=\"%d\" Chunks=\"%d\" Url=\"QualityLevels({bitrate})/Fragments(audio={start time})\">\n", audio_streams, audio_chunks);
        for (i = 0; i < s->nb_streams; i++) {
            OutputStream *os = &c->streams[i];
            if (s->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;
            last = i;
            avio_printf(out, "<QualityLevel Index=\"%d\" Bitrate=\"%"PRId64"\" FourCC=\"%s\" SamplingRate=\"%d\" Channels=\"%d\" BitsPerSample=\"16\" PacketSize=\"%d\" AudioTag=\"%d\" CodecPrivateData=\"%s\" />\n",
                        index, s->streams[i]->codecpar->bit_rate, os->fourcc, s->streams[i]->codecpar->sample_rate,
                        s->streams[i]->codecpar->ch_layout.nb_channels, os->packet_size, os->audio_tag, os->private_str);
            index++;
        }
        output_chunk_list(&c->streams[last], out, final, c->lookahead_count, c->window_size);
        avio_printf(out, "</StreamIndex>\n");
    }
    avio_printf(out, "</SmoothStreamingMedia>\n");
    avio_flush(out);
    ff_format_io_close(s, &out);
    return ff_rename(temp_filename, filename, s);
}

static int ism_write_header(AVFormatContext *s)
{
    SmoothStreamingContext *c = s->priv_data;
    int ret = 0, i;
    const AVOutputFormat *oformat;

    if (mkdir(s->url, 0777) == -1 && errno != EEXIST) {
        av_log(s, AV_LOG_ERROR, "mkdir failed\n");
        return AVERROR(errno);
    }

    oformat = av_guess_format("ismv", NULL, NULL);
    if (!oformat) {
        return AVERROR_MUXER_NOT_FOUND;
    }

    c->streams = av_calloc(s->nb_streams, sizeof(*c->streams));
    if (!c->streams) {
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < s->nb_streams; i++) {
        OutputStream *os = &c->streams[i];
        AVFormatContext *ctx;
        AVStream *st;
        AVDictionary *opts = NULL;

        if (!s->streams[i]->codecpar->bit_rate) {
            av_log(s, AV_LOG_WARNING, "No bit rate set for stream %d\n", i);
            // create a tmp name for the directory of fragments
            snprintf(os->dirname, sizeof(os->dirname), "%s/QualityLevels(Tmp_%d)", s->url, i);
        } else {
            snprintf(os->dirname, sizeof(os->dirname), "%s/QualityLevels(%"PRId64")", s->url, s->streams[i]->codecpar->bit_rate);
        }

        if (mkdir(os->dirname, 0777) == -1 && errno != EEXIST) {
            av_log(s, AV_LOG_ERROR, "mkdir failed\n");
            return AVERROR(errno);
        }

        os->ctx = ctx = avformat_alloc_context();
        if (!ctx) {
            return AVERROR(ENOMEM);
        }
        if ((ret = ff_copy_whiteblacklists(ctx, s)) < 0)
            return ret;
        ctx->oformat = oformat;
        ctx->interrupt_callback = s->interrupt_callback;

        if (!(st = avformat_new_stream(ctx, NULL))) {
            return AVERROR(ENOMEM);
        }
        if ((ret = avcodec_parameters_copy(st->codecpar, s->streams[i]->codecpar)) < 0) {
            return ret;
        }
        st->sample_aspect_ratio = s->streams[i]->sample_aspect_ratio;
        st->time_base = s->streams[i]->time_base;

        ctx->pb = avio_alloc_context(os->iobuf, sizeof(os->iobuf), 1, os, NULL, ism_write, ism_seek);
        if (!ctx->pb) {
            return AVERROR(ENOMEM);
        }

        av_dict_set_int(&opts, "ism_lookahead", c->lookahead_count, 0);
        av_dict_set(&opts, "movflags", "+frag_custom", 0);
        ret = avformat_write_header(ctx, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
             return ret;
        }
        avio_flush(ctx->pb);
        s->streams[i]->time_base = st->time_base;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            c->has_video = 1;
            os->stream_type_tag = "video";
            if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
                os->fourcc = "H264";
            } else if (st->codecpar->codec_id == AV_CODEC_ID_VC1) {
                os->fourcc = "WVC1";
            } else {
                av_log(s, AV_LOG_ERROR, "Unsupported video codec\n");
                return AVERROR(EINVAL);
            }
        } else {
            c->has_audio = 1;
            os->stream_type_tag = "audio";
            if (st->codecpar->codec_id == AV_CODEC_ID_AAC) {
                os->fourcc = "AACL";
                os->audio_tag = 0xff;
            } else if (st->codecpar->codec_id == AV_CODEC_ID_WMAPRO) {
                os->fourcc = "WMAP";
                os->audio_tag = 0x0162;
            } else {
                av_log(s, AV_LOG_ERROR, "Unsupported audio codec\n");
                return AVERROR(EINVAL);
            }
            os->packet_size = st->codecpar->block_align ? st->codecpar->block_align : 4;
        }
        get_private_data(os);
    }

    if (!c->has_video && c->min_frag_duration <= 0) {
        av_log(s, AV_LOG_WARNING, "no video stream and no min frag duration set\n");
        return AVERROR(EINVAL);
    }
    ret = write_manifest(s, 0);
    if (ret < 0)
        return ret;

    return 0;
}

static int parse_fragment(AVFormatContext *s, const char *filename, int64_t *start_ts, int64_t *duration, int64_t *moof_size, int64_t size)
{
    AVIOContext *in;
    int ret;
    uint32_t len;
    if ((ret = s->io_open(s, &in, filename, AVIO_FLAG_READ, NULL)) < 0)
        return ret;
    ret = AVERROR(EIO);
    *moof_size = avio_rb32(in);
    if (*moof_size < 8 || *moof_size > size)
        goto fail;
    if (avio_rl32(in) != MKTAG('m','o','o','f'))
        goto fail;
    len = avio_rb32(in);
    if (len > *moof_size)
        goto fail;
    if (avio_rl32(in) != MKTAG('m','f','h','d'))
        goto fail;
    avio_seek(in, len - 8, SEEK_CUR);
    avio_rb32(in); /* traf size */
    if (avio_rl32(in) != MKTAG('t','r','a','f'))
        goto fail;
    while (avio_tell(in) < *moof_size) {
        uint32_t len = avio_rb32(in);
        uint32_t tag = avio_rl32(in);
        int64_t end = avio_tell(in) + len - 8;
        if (len < 8 || len >= *moof_size)
            goto fail;
        if (tag == MKTAG('u','u','i','d')) {
            static const AVUUID tfxd = {
                0x6d, 0x1d, 0x9b, 0x05, 0x42, 0xd5, 0x44, 0xe6,
                0x80, 0xe2, 0x14, 0x1d, 0xaf, 0xf7, 0x57, 0xb2
            };
            AVUUID uuid;
            avio_read(in, uuid, 16);
            if (av_uuid_equal(uuid, tfxd) && len >= 8 + 16 + 4 + 16) {
                avio_seek(in, 4, SEEK_CUR);
                *start_ts = avio_rb64(in);
                *duration = avio_rb64(in);
                ret = 0;
                break;
            }
        }
        avio_seek(in, end, SEEK_SET);
    }
fail:
    ff_format_io_close(s, &in);
    return ret;
}

static int add_fragment(OutputStream *os, const char *file, const char *infofile, int64_t start_time, int64_t duration, int64_t start_pos, int64_t size)
{
    int err;
    Fragment *frag;
    if (os->nb_fragments >= os->fragments_size) {
        os->fragments_size = (os->fragments_size + 1) * 2;
        if ((err = av_reallocp_array(&os->fragments, sizeof(*os->fragments),
                               os->fragments_size)) < 0) {
            os->fragments_size = 0;
            os->nb_fragments = 0;
            return err;
        }
    }
    frag = av_mallocz(sizeof(*frag));
    if (!frag)
        return AVERROR(ENOMEM);
    av_strlcpy(frag->file, file, sizeof(frag->file));
    av_strlcpy(frag->infofile, infofile, sizeof(frag->infofile));
    frag->start_time = start_time;
    frag->duration = duration;
    frag->start_pos = start_pos;
    frag->size = size;
    frag->n = os->fragment_index;
    os->fragments[os->nb_fragments++] = frag;
    os->fragment_index++;
    return 0;
}

static int copy_moof(AVFormatContext *s, const char* infile, const char *outfile, int64_t size)
{
    AVIOContext *in, *out;
    int ret = 0;
    if ((ret = s->io_open(s, &in, infile, AVIO_FLAG_READ, NULL)) < 0)
        return ret;
    if ((ret = s->io_open(s, &out, outfile, AVIO_FLAG_WRITE, NULL)) < 0) {
        ff_format_io_close(s, &in);
        return ret;
    }
    while (size > 0) {
        uint8_t buf[8192];
        int n = FFMIN(size, sizeof(buf));
        n = avio_read(in, buf, n);
        if (n <= 0) {
            ret = AVERROR(EIO);
            break;
        }
        avio_write(out, buf, n);
        size -= n;
    }
    avio_flush(out);
    ff_format_io_close(s, &out);
    ff_format_io_close(s, &in);
    return ret;
}

static int ism_flush(AVFormatContext *s, int final)
{
    SmoothStreamingContext *c = s->priv_data;
    int i, ret = 0;

    for (i = 0; i < s->nb_streams; i++) {
        OutputStream *os = &c->streams[i];
        char filename[1024], target_filename[1024], header_filename[1024], curr_dirname[1024];
        int64_t size;
        int64_t start_ts, duration, moof_size;
        if (!os->packets_written)
            continue;

        snprintf(filename, sizeof(filename), "%s/temp", os->dirname);
        ret = ffurl_open_whitelist(&os->out, filename, AVIO_FLAG_WRITE, &s->interrupt_callback, NULL, s->protocol_whitelist, s->protocol_blacklist, NULL);
        if (ret < 0)
            break;
        os->cur_start_pos = os->tail_pos;
        av_write_frame(os->ctx, NULL);
        avio_flush(os->ctx->pb);
        os->packets_written = 0;
        if (!os->out || os->tail_out)
            return AVERROR(EIO);

        ffurl_closep(&os->out);
        size = os->tail_pos - os->cur_start_pos;
        if ((ret = parse_fragment(s, filename, &start_ts, &duration, &moof_size, size)) < 0)
            break;

        if (!s->streams[i]->codecpar->bit_rate) {
            int64_t bitrate = (int64_t) size * 8 * AV_TIME_BASE / av_rescale_q(duration, s->streams[i]->time_base, AV_TIME_BASE_Q);
            if (!bitrate) {
                av_log(s, AV_LOG_ERROR, "calculating bitrate got zero.\n");
                ret = AVERROR(EINVAL);
                return ret;
            }

            av_log(s, AV_LOG_DEBUG, "calculated bitrate: %"PRId64"\n", bitrate);
            s->streams[i]->codecpar->bit_rate = bitrate;
            memcpy(curr_dirname, os->dirname, sizeof(os->dirname));
            snprintf(os->dirname, sizeof(os->dirname), "%s/QualityLevels(%"PRId64")", s->url, s->streams[i]->codecpar->bit_rate);
            snprintf(filename, sizeof(filename), "%s/temp", os->dirname);

            // rename the tmp folder back to the correct name since we now have the bitrate
            if ((ret = ff_rename((const char*)curr_dirname,  os->dirname, s)) < 0)
                return ret;
        }

        snprintf(header_filename, sizeof(header_filename), "%s/FragmentInfo(%s=%"PRIu64")", os->dirname, os->stream_type_tag, start_ts);
        snprintf(target_filename, sizeof(target_filename), "%s/Fragments(%s=%"PRIu64")", os->dirname, os->stream_type_tag, start_ts);
        copy_moof(s, filename, header_filename, moof_size);
        ret = ff_rename(filename, target_filename, s);
        if (ret < 0)
            break;
        add_fragment(os, target_filename, header_filename, start_ts, duration,
                     os->cur_start_pos, size);
    }

    if (c->window_size || (final && c->remove_at_exit)) {
        for (i = 0; i < s->nb_streams; i++) {
            OutputStream *os = &c->streams[i];
            int j;
            int remove = os->nb_fragments - c->window_size - c->extra_window_size - c->lookahead_count;
            if (final && c->remove_at_exit)
                remove = os->nb_fragments;
            if (remove > 0) {
                for (j = 0; j < remove; j++) {
                    unlink(os->fragments[j]->file);
                    unlink(os->fragments[j]->infofile);
                    av_freep(&os->fragments[j]);
                }
                os->nb_fragments -= remove;
                memmove(os->fragments, os->fragments + remove, os->nb_fragments * sizeof(*os->fragments));
            }
            if (final && c->remove_at_exit)
                rmdir(os->dirname);
        }
    }

    if (ret >= 0)
        ret = write_manifest(s, final);
    return ret;
}

static int ism_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SmoothStreamingContext *c = s->priv_data;
    AVStream *st = s->streams[pkt->stream_index];
    FFStream *const sti = ffstream(st);
    OutputStream *os = &c->streams[pkt->stream_index];
    int64_t end_dts = (c->nb_fragments + 1) * (int64_t) c->min_frag_duration;
    int ret;

    if (sti->first_dts == AV_NOPTS_VALUE)
        sti->first_dts = pkt->dts;

    if ((!c->has_video || st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) &&
        av_compare_ts(pkt->dts - sti->first_dts, st->time_base,
                      end_dts, AV_TIME_BASE_Q) >= 0 &&
        pkt->flags & AV_PKT_FLAG_KEY && os->packets_written) {

        if ((ret = ism_flush(s, 0)) < 0)
            return ret;
        c->nb_fragments++;
    }

    os->packets_written++;
    return ff_write_chained(os->ctx, 0, pkt, s, 0);
}

static int ism_write_trailer(AVFormatContext *s)
{
    SmoothStreamingContext *c = s->priv_data;
    ism_flush(s, 1);

    if (c->remove_at_exit) {
        char filename[1024];
        snprintf(filename, sizeof(filename), "%s/Manifest", s->url);
        unlink(filename);
        rmdir(s->url);
    }

    return 0;
}

#define OFFSET(x) offsetof(SmoothStreamingContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "window_size", "number of fragments kept in the manifest", OFFSET(window_size), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, E },
    { "extra_window_size", "number of fragments kept outside of the manifest before removing from disk", OFFSET(extra_window_size), AV_OPT_TYPE_INT, { .i64 = 5 }, 0, INT_MAX, E },
    { "lookahead_count", "number of lookahead fragments", OFFSET(lookahead_count), AV_OPT_TYPE_INT, { .i64 = 2 }, 0, INT_MAX, E },
    { "min_frag_duration", "minimum fragment duration (in microseconds)", OFFSET(min_frag_duration), AV_OPT_TYPE_INT64, { .i64 = 5000000 }, 0, INT_MAX, E },
    { "remove_at_exit", "remove all fragments when finished", OFFSET(remove_at_exit), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, E },
    { NULL },
};

static const AVClass ism_class = {
    .class_name = "smooth streaming muxer",
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};


const FFOutputFormat ff_smoothstreaming_muxer = {
    .p.name         = "smoothstreaming",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Smooth Streaming Muxer"),
    .p.audio_codec  = AV_CODEC_ID_AAC,
    .p.video_codec  = AV_CODEC_ID_H264,
    .p.flags        = AVFMT_GLOBALHEADER | AVFMT_NOFILE,
    .p.priv_class   = &ism_class,
    .priv_data_size = sizeof(SmoothStreamingContext),
    .write_header   = ism_write_header,
    .write_packet   = ism_write_packet,
    .write_trailer  = ism_write_trailer,
    .deinit         = ism_free,
};
