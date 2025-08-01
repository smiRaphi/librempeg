/*
 * Tee pseudo-muxer
 * Copyright (c) 2012 Nicolas George
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
#include "libavutil/avstring.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavcodec/bsf.h"
#include "internal.h"
#include "avformat.h"
#include "mux.h"
#include "tee_common.h"

typedef enum {
    ON_SLAVE_FAILURE_ABORT  = 1,
    ON_SLAVE_FAILURE_IGNORE = 2
} SlaveFailurePolicy;

#define DEFAULT_SLAVE_FAILURE_POLICY ON_SLAVE_FAILURE_ABORT

typedef struct {
    AVFormatContext *avf;
    AVBSFContext **bsfs; ///< bitstream filters per stream

    SlaveFailurePolicy on_fail;
    int use_fifo;
    AVDictionary *fifo_options;

    /** map from input to output streams indexes,
     * disabled output streams are set to -1 */
    int *stream_map;
    int header_written;
} TeeSlave;

typedef struct TeeContext {
    const AVClass *class;
    unsigned nb_slaves;
    unsigned nb_alive;
    TeeSlave *slaves;
    int use_fifo;
    AVDictionary *fifo_options;
} TeeContext;

static const char *const slave_delim     = "|";
static const char *const slave_bsfs_spec_sep = "/";
static const char *const slave_select_sep = ",";

#define OFFSET(x) offsetof(TeeContext, x)
static const AVOption options[] = {
        {"use_fifo", "Use fifo pseudo-muxer to separate actual muxers from encoder",
         OFFSET(use_fifo), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, AV_OPT_FLAG_ENCODING_PARAM},
        {"fifo_options", "fifo pseudo-muxer options", OFFSET(fifo_options),
         AV_OPT_TYPE_DICT, {.str = NULL}, 0, 0, AV_OPT_FLAG_ENCODING_PARAM},
        {NULL}
};

static const AVClass tee_muxer_class = {
    .class_name = "Tee muxer",
    .option = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static inline int parse_slave_failure_policy_option(const char *opt, TeeSlave *tee_slave)
{
    if (!opt) {
        tee_slave->on_fail = DEFAULT_SLAVE_FAILURE_POLICY;
        return 0;
    } else if (!av_strcasecmp("abort", opt)) {
        tee_slave->on_fail = ON_SLAVE_FAILURE_ABORT;
        return 0;
    } else if (!av_strcasecmp("ignore", opt)) {
        tee_slave->on_fail = ON_SLAVE_FAILURE_IGNORE;
        return 0;
    }
    /* Set failure behaviour to abort, so invalid option error will not be ignored */
    tee_slave->on_fail = ON_SLAVE_FAILURE_ABORT;
    return AVERROR(EINVAL);
}

static int parse_slave_fifo_policy(const char *use_fifo, TeeSlave *tee_slave)
{
    /*TODO - change this to use proper function for parsing boolean
     *       options when there is one */
    if (av_match_name(use_fifo, "true,y,yes,enable,enabled,on,1")) {
        tee_slave->use_fifo = 1;
    } else if (av_match_name(use_fifo, "false,n,no,disable,disabled,off,0")) {
        tee_slave->use_fifo = 0;
    } else {
        return AVERROR(EINVAL);
    }
    return 0;
}

static int parse_slave_fifo_options(const char *fifo_options, TeeSlave *tee_slave)
{
    return av_dict_parse_string(&tee_slave->fifo_options, fifo_options, "=", ":", 0);
}

static int close_slave(TeeSlave *tee_slave)
{
    AVFormatContext *avf;
    int ret = 0;

    av_dict_free(&tee_slave->fifo_options);
    avf = tee_slave->avf;
    if (!avf)
        return 0;

    if (tee_slave->header_written)
        ret = av_write_trailer(avf);

    if (tee_slave->bsfs) {
        for (unsigned i = 0; i < avf->nb_streams; ++i)
            av_bsf_free(&tee_slave->bsfs[i]);
    }
    av_freep(&tee_slave->stream_map);
    av_freep(&tee_slave->bsfs);

    ff_format_io_close(avf, &avf->pb);
    avformat_free_context(avf);
    tee_slave->avf = NULL;
    return ret;
}

static void close_slaves(AVFormatContext *avf)
{
    TeeContext *tee = avf->priv_data;

    for (unsigned i = 0; i < tee->nb_slaves; i++) {
        close_slave(&tee->slaves[i]);
    }
    av_freep(&tee->slaves);
}

static int open_slave(AVFormatContext *avf, char *slave, TeeSlave *tee_slave)
{
    int ret;
    AVDictionary *options = NULL, *bsf_options = NULL;
    const AVDictionaryEntry *entry;
    char *filename;
    char *format = NULL, *select = NULL;
    AVFormatContext *avf2 = NULL;
    int stream_count;
    int fullret;
    char *subselect = NULL, *next_subselect = NULL, *first_subselect = NULL, *tmp_select = NULL;

    if ((ret = ff_tee_parse_slave_options(avf, slave, &options, &filename)) < 0)
        return ret;

#define CONSUME_OPTION(option, field, action) do {                      \
        AVDictionaryEntry *en = av_dict_get(options, option, NULL, 0);  \
        if (en) {                                                       \
            field = en->value;                                          \
            { action }                                                  \
            av_dict_set(&options, option, NULL, 0);                     \
        }                                                               \
    } while (0)
#define STEAL_OPTION(option, field)                                     \
    CONSUME_OPTION(option, field,                                       \
                   en->value = NULL; /* prevent it from being freed */)
#define PROCESS_OPTION(option, function, on_error) do {                 \
        const char *value;                                              \
        CONSUME_OPTION(option, value, if ((ret = function) < 0)         \
                                          { { on_error } goto end; });  \
    } while (0)

    STEAL_OPTION("f", format);
    STEAL_OPTION("select", select);
    PROCESS_OPTION("onfail",
                   parse_slave_failure_policy_option(value, tee_slave),
                   av_log(avf, AV_LOG_ERROR, "Invalid onfail option value, "
                          "valid options are 'abort' and 'ignore'\n"););
    PROCESS_OPTION("use_fifo",
                   parse_slave_fifo_policy(value, tee_slave),
                   av_log(avf, AV_LOG_ERROR, "Error parsing fifo options: %s\n",
                          av_err2str(ret)););
    PROCESS_OPTION("fifo_options",
                   parse_slave_fifo_options(value, tee_slave), ;);
    entry = NULL;
    while ((entry = av_dict_get(options, "bsfs", NULL, AV_DICT_IGNORE_SUFFIX))) {
        /* trim out strlen("bsfs") characters from key */
        av_dict_set(&bsf_options, entry->key + 4, entry->value, 0);
        av_dict_set(&options, entry->key, NULL, 0);
    }

    if (tee_slave->use_fifo) {

        if (options) {
            char *format_options_str = NULL;
            ret = av_dict_get_string(options, &format_options_str, '=', ':');
            if (ret < 0)
                goto end;

            ret = av_dict_set(&tee_slave->fifo_options, "format_opts", format_options_str,
                              AV_DICT_DONT_STRDUP_VAL);
            if (ret < 0)
                goto end;
        }

        if (format) {
            ret = av_dict_set(&tee_slave->fifo_options, "fifo_format", format,
                              AV_DICT_DONT_STRDUP_VAL);
            format = NULL;
            if (ret < 0)
                goto end;
        }

        av_dict_free(&options);
        options = tee_slave->fifo_options;
        tee_slave->fifo_options = NULL;
    }
    ret = avformat_alloc_output_context2(&avf2, NULL,
                                         tee_slave->use_fifo ? "fifo" :format, filename);
    if (ret < 0)
        goto end;
    tee_slave->avf = avf2;
    av_dict_copy(&avf2->metadata, avf->metadata, 0);
    avf2->opaque   = avf->opaque;
    avf2->io_open  = avf->io_open;
    avf2->io_close2 = avf->io_close2;
    avf2->interrupt_callback = avf->interrupt_callback;
    avf2->flags = avf->flags;
    avf2->strict_std_compliance = avf->strict_std_compliance;

    tee_slave->stream_map = av_calloc(avf->nb_streams, sizeof(*tee_slave->stream_map));
    if (!tee_slave->stream_map) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    stream_count = 0;
    for (unsigned i = 0; i < avf->nb_streams; i++) {
        const AVStream *st = avf->streams[i];
        AVStream *st2;
        if (select) {
            tmp_select = av_strdup(select);  // av_strtok is destructive so we regenerate it in each loop
            if (!tmp_select) {
                ret = AVERROR(ENOMEM);
                goto end;
            }
            fullret = 0;
            first_subselect = tmp_select;
            next_subselect = NULL;
            while (subselect = av_strtok(first_subselect, slave_select_sep, &next_subselect)) {
                first_subselect = NULL;

                ret = avformat_match_stream_specifier(avf, avf->streams[i], subselect);
                if (ret < 0) {
                    av_log(avf, AV_LOG_ERROR,
                           "Invalid stream specifier '%s' for output '%s'\n",
                           subselect, slave);
                    goto end;
                }
                if (ret != 0) {
                    fullret = 1; // match
                    break;
                }
            }
            av_freep(&tmp_select);

            if (fullret == 0) { /* no match */
                tee_slave->stream_map[i] = -1;
                continue;
            }
        }
        tee_slave->stream_map[i] = stream_count++;

        st2 = ff_stream_clone(avf2, st);
        if (!st2) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    ret = ff_format_output_open(avf2, filename, &options);
    if (ret < 0) {
        av_log(avf, AV_LOG_ERROR, "Slave '%s': error opening: %s\n", slave,
               av_err2str(ret));
        goto end;
    }

    if ((ret = avformat_write_header(avf2, &options)) < 0) {
        av_log(avf, AV_LOG_ERROR, "Slave '%s': error writing header: %s\n",
               slave, av_err2str(ret));
        goto end;
    }
    tee_slave->header_written = 1;

    tee_slave->bsfs = av_calloc(avf2->nb_streams, sizeof(*tee_slave->bsfs));
    if (!tee_slave->bsfs) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    entry = NULL;
    while (entry = av_dict_iterate(bsf_options, NULL)) {
        const char *spec = entry->key;
        if (*spec) {
            if (strspn(spec, slave_bsfs_spec_sep) != 1) {
                av_log(avf, AV_LOG_ERROR,
                       "Specifier separator in '%s' is '%c', but only characters '%s' "
                       "are allowed\n", entry->key, *spec, slave_bsfs_spec_sep);
                ret = AVERROR(EINVAL);
                goto end;
            }
            spec++; /* consume separator */
        }

        for (unsigned i = 0; i < avf2->nb_streams; i++) {
            ret = avformat_match_stream_specifier(avf2, avf2->streams[i], spec);
            if (ret < 0) {
                av_log(avf, AV_LOG_ERROR,
                       "Invalid stream specifier '%s' in bsfs option '%s' for slave "
                       "output '%s'\n", spec, entry->key, filename);
                goto end;
            }

            if (ret > 0) {
                av_log(avf, AV_LOG_DEBUG, "spec:%s bsfs:%s matches stream %d of slave "
                       "output '%s'\n", spec, entry->value, i, filename);
                if (tee_slave->bsfs[i]) {
                    av_log(avf, AV_LOG_WARNING,
                           "Duplicate bsfs specification associated to stream %d of slave "
                           "output '%s', filters will be ignored\n", i, filename);
                    continue;
                }
                ret = av_bsf_list_parse_str(entry->value, &tee_slave->bsfs[i]);
                if (ret < 0) {
                    av_log(avf, AV_LOG_ERROR,
                           "Error parsing bitstream filter sequence '%s' associated to "
                           "stream %d of slave output '%s'\n", entry->value, i, filename);
                    goto end;
                }
            }
        }

        av_dict_set(&bsf_options, entry->key, NULL, 0);
    }

    for (unsigned i = 0; i < avf->nb_streams; i++){
        int target_stream = tee_slave->stream_map[i];
        if (target_stream < 0)
            continue;

        if (!tee_slave->bsfs[target_stream]) {
            /* Add pass-through bitstream filter */
            ret = av_bsf_get_null_filter(&tee_slave->bsfs[target_stream]);
            if (ret < 0) {
                av_log(avf, AV_LOG_ERROR,
                       "Failed to create pass-through bitstream filter: %s\n",
                       av_err2str(ret));
                goto end;
            }
        }

        tee_slave->bsfs[target_stream]->time_base_in = avf->streams[i]->time_base;
        ret = avcodec_parameters_copy(tee_slave->bsfs[target_stream]->par_in,
                                      avf->streams[i]->codecpar);
        if (ret < 0)
            goto end;

        ret = av_bsf_init(tee_slave->bsfs[target_stream]);
        if (ret < 0) {
            av_log(avf, AV_LOG_ERROR,
            "Failed to initialize bitstream filter(s): %s\n",
            av_err2str(ret));
            goto end;
        }
    }

    if (options) {
        entry = NULL;
        while ((entry = av_dict_iterate(options, entry)))
            av_log(avf2, AV_LOG_ERROR, "Unknown option '%s'\n", entry->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto end;
    }

end:
    av_free(format);
    av_free(select);
    av_dict_free(&options);
    av_dict_free(&bsf_options);
    av_freep(&tmp_select);
    return ret;
}

static void log_slave(TeeSlave *slave, void *log_ctx, int log_level)
{
    av_log(log_ctx, log_level, "filename:'%s' format:%s\n",
           slave->avf->url, slave->avf->oformat->name);
    for (unsigned i = 0; i < slave->avf->nb_streams; i++) {
        AVStream *st = slave->avf->streams[i];
        AVBSFContext *bsf = slave->bsfs[i];
        const char *bsf_name;

        av_log(log_ctx, log_level, "    stream:%d codec:%s type:%s",
               i, avcodec_get_name(st->codecpar->codec_id),
               av_get_media_type_string(st->codecpar->codec_type));

        bsf_name = bsf->filter->priv_class ?
                   bsf->filter->priv_class->item_name(bsf) : bsf->filter->name;
        av_log(log_ctx, log_level, " bsfs: %s\n", bsf_name);
    }
}

static int tee_process_slave_failure(AVFormatContext *avf, unsigned slave_idx, int err_n)
{
    TeeContext *tee = avf->priv_data;
    TeeSlave *tee_slave = &tee->slaves[slave_idx];

    tee->nb_alive--;

    close_slave(tee_slave);

    if (!tee->nb_alive) {
        av_log(avf, AV_LOG_ERROR, "All tee outputs failed.\n");
        return err_n;
    } else if (tee_slave->on_fail == ON_SLAVE_FAILURE_ABORT) {
        av_log(avf, AV_LOG_ERROR, "Slave muxer #%u failed, aborting.\n", slave_idx);
        return err_n;
    } else {
        av_log(avf, AV_LOG_ERROR, "Slave muxer #%u failed: %s, continuing with %u/%u slaves.\n",
               slave_idx, av_err2str(err_n), tee->nb_alive, tee->nb_slaves);
        return 0;
    }
}

static int tee_write_header(AVFormatContext *avf)
{
    TeeContext *tee = avf->priv_data;
    unsigned nb_slaves = 0;
    const char *filename = avf->url;
    char **slaves = NULL;
    int ret;

    while (*filename) {
        char *slave = av_get_token(&filename, slave_delim);
        if (!slave) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = av_dynarray_add_nofree(&slaves, &nb_slaves, slave);
        if (ret < 0) {
            av_free(slave);
            goto fail;
        }
        if (strspn(filename, slave_delim))
            filename++;
    }

    if (!FF_ALLOCZ_TYPED_ARRAY(tee->slaves, nb_slaves)) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    tee->nb_slaves = tee->nb_alive = nb_slaves;

    for (unsigned i = 0; i < nb_slaves; i++) {

        tee->slaves[i].use_fifo = tee->use_fifo;
        ret = av_dict_copy(&tee->slaves[i].fifo_options, tee->fifo_options, 0);
        if (ret < 0)
            goto fail;

        if ((ret = open_slave(avf, slaves[i], &tee->slaves[i])) < 0) {
            ret = tee_process_slave_failure(avf, i, ret);
            if (ret < 0)
                goto fail;
        } else {
            log_slave(&tee->slaves[i], avf, AV_LOG_VERBOSE);
        }
        av_freep(&slaves[i]);
    }

    for (unsigned i = 0; i < avf->nb_streams; i++) {
        int mapped = 0;
        for (unsigned j = 0; j < tee->nb_slaves; j++)
            if (tee->slaves[j].avf)
                mapped += tee->slaves[j].stream_map[i] >= 0;
        if (!mapped)
            av_log(avf, AV_LOG_WARNING, "Input stream #%d is not mapped "
                   "to any slave.\n", i);
    }
    av_free(slaves);
    return 0;

fail:
    for (unsigned i = 0; i < nb_slaves; i++)
        av_freep(&slaves[i]);
    close_slaves(avf);
    av_free(slaves);
    return ret;
}

static int tee_write_trailer(AVFormatContext *avf)
{
    TeeContext *tee = avf->priv_data;
    int ret_all = 0, ret;

    for (unsigned i = 0; i < tee->nb_slaves; i++) {
        if ((ret = close_slave(&tee->slaves[i])) < 0) {
            ret = tee_process_slave_failure(avf, i, ret);
            if (!ret_all && ret < 0)
                ret_all = ret;
        }
    }
    av_freep(&tee->slaves);
    return ret_all;
}

static int tee_write_packet(AVFormatContext *avf, AVPacket *pkt)
{
    TeeContext *tee = avf->priv_data;
    AVPacket *const pkt2 = ffformatcontext(avf)->pkt;
    int ret_all = 0, ret;
    unsigned s;
    int s2;

    for (unsigned i = 0; i < tee->nb_slaves; i++) {
        AVFormatContext *avf2 = tee->slaves[i].avf;
        AVBSFContext *bsfs;

        if (!avf2)
            continue;

        /* Flush slave if pkt is NULL*/
        if (!pkt) {
            ret = av_interleaved_write_frame(avf2, NULL);
            if (ret < 0) {
                ret = tee_process_slave_failure(avf, i, ret);
                if (!ret_all && ret < 0)
                    ret_all = ret;
            }
            continue;
        }

        s = pkt->stream_index;
        s2 = tee->slaves[i].stream_map[s];
        if (s2 < 0)
            continue;

        if ((ret = av_packet_ref(pkt2, pkt)) < 0) {
            if (!ret_all)
                ret_all = ret;
            continue;
        }
        bsfs = tee->slaves[i].bsfs[s2];
        pkt2->stream_index = s2;

        ret = av_bsf_send_packet(bsfs, pkt2);
        if (ret < 0) {
            av_packet_unref(pkt2);
            av_log(avf, AV_LOG_ERROR, "Error while sending packet to bitstream filter: %s\n",
                   av_err2str(ret));
            ret = tee_process_slave_failure(avf, i, ret);
            if (!ret_all && ret < 0)
                ret_all = ret;
        }

        while(1) {
            ret = av_bsf_receive_packet(bsfs, pkt2);
            if (ret == AVERROR(EAGAIN)) {
                ret = 0;
                break;
            } else if (ret < 0) {
                break;
            }

            av_packet_rescale_ts(pkt2, bsfs->time_base_out,
                                 avf2->streams[s2]->time_base);
            ret = av_interleaved_write_frame(avf2, pkt2);
            if (ret < 0)
                break;
        };

        if (ret < 0) {
            ret = tee_process_slave_failure(avf, i, ret);
            if (!ret_all && ret < 0)
                ret_all = ret;
        }
    }
    return ret_all;
}

const FFOutputFormat ff_tee_muxer = {
    .p.name            = "tee",
    .p.long_name       = NULL_IF_CONFIG_SMALL("Multiple muxer tee"),
    .priv_data_size    = sizeof(TeeContext),
    .write_header      = tee_write_header,
    .write_trailer     = tee_write_trailer,
    .write_packet      = tee_write_packet,
    .p.priv_class      = &tee_muxer_class,
    .p.flags           = AVFMT_NOFILE | AVFMT_TS_NEGATIVE,
    .flags_internal    = FF_OFMT_FLAG_ALLOW_FLUSH,
};
