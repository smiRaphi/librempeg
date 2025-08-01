/*
 * MPEG-2 transport stream (aka DVB) demuxer
 * Copyright (c) 2002-2003 Fabrice Bellard
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

#include "config_components.h"

#include "libavutil/attributes_internal.h"
#include "libavutil/buffer.h"
#include "libavutil/crc.h"
#include "libavutil/internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/dict.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavutil/dovi_meta.h"
#include "libavcodec/bytestream.h"
#include "libavcodec/defs.h"
#include "libavcodec/get_bits.h"
#include "libavcodec/opus/opus.h"
#include "avformat.h"
#include "mpegts.h"
#include "internal.h"
#include "avio_internal.h"
#include "demux.h"
#include "mpeg.h"
#include "isom.h"
#if CONFIG_ICONV
#include <iconv.h>
#endif

/* maximum size in which we look for synchronization if
 * synchronization is lost */
#define MAX_RESYNC_SIZE 65536

#define MAX_MP4_DESCR_COUNT 16

#define MOD_UNLIKELY(modulus, dividend, divisor, prev_dividend)                \
    do {                                                                       \
        if ((prev_dividend) == 0 || (dividend) - (prev_dividend) != (divisor)) \
            (modulus) = (dividend) % (divisor);                                \
        (prev_dividend) = (dividend);                                          \
    } while (0)

#define PROBE_PACKET_MAX_BUF 8192
#define PROBE_PACKET_MARGIN 5

enum MpegTSFilterType {
    MPEGTS_PES,
    MPEGTS_SECTION,
    MPEGTS_PCR,
};

typedef struct MpegTSFilter MpegTSFilter;

typedef int PESCallback (MpegTSFilter *f, const uint8_t *buf, int len,
                         int is_start, int64_t pos);

typedef struct MpegTSPESFilter {
    PESCallback *pes_cb;
    void *opaque;
} MpegTSPESFilter;

typedef void SectionCallback (MpegTSFilter *f, const uint8_t *buf, int len);

typedef void SetServiceCallback (void *opaque, int ret);

typedef struct MpegTSSectionFilter {
    int section_index;
    int section_h_size;
    int last_ver;
    unsigned crc;
    unsigned last_crc;
    uint8_t *section_buf;
    unsigned int check_crc : 1;
    unsigned int end_of_section_reached : 1;
    SectionCallback *section_cb;
    void *opaque;
} MpegTSSectionFilter;

struct MpegTSFilter {
    int pid;
    int es_id;
    int last_cc; /* last cc code (-1 if first packet) */
    int64_t last_pcr;
    int discard;
    enum MpegTSFilterType type;
    union {
        MpegTSPESFilter pes_filter;
        MpegTSSectionFilter section_filter;
    } u;
};

struct Stream {
    int idx;
    int stream_identifier;
};

#define MAX_STREAMS_PER_PROGRAM 128
#define MAX_PIDS_PER_PROGRAM (MAX_STREAMS_PER_PROGRAM + 2)
struct Program {
    unsigned int id; // program id/service id
    unsigned int nb_pids;
    unsigned int pids[MAX_PIDS_PER_PROGRAM];
    unsigned int nb_streams;
    struct Stream streams[MAX_STREAMS_PER_PROGRAM];

    /** have we found pmt for this program */
    int pmt_found;
};

struct MpegTSContext {
    const AVClass *class;
    /* user data */
    AVFormatContext *stream;
    /** raw packet size, including FEC if present */
    int raw_packet_size;

    int64_t pos47_full;

    /** if true, all pids are analyzed to find streams */
    int auto_guess;

    /** compute exact PCR for each transport stream packet */
    int mpeg2ts_compute_pcr;

    /** fix dvb teletext pts                                 */
    int fix_teletext_pts;

    int64_t cur_pcr;    /**< used to estimate the exact PCR */
    int64_t pcr_incr;   /**< used to estimate the exact PCR */

    /* data needed to handle file based ts */
    /** stop parsing loop */
    int stop_parse;
    /** packet containing Audio/Video data */
    AVPacket *pkt;
    /** to detect seek */
    int64_t last_pos;

    int skip_changes;
    int skip_clear;
    int skip_unknown_pmt;

    int scan_all_pmts;

    int resync_size;
    int merge_pmt_versions;
    int max_packet_size;

    int id;

    /******************************************/
    /* private mpegts data */
    /* scan context */
    /** structure to keep track of Program->pids mapping */
    unsigned int nb_prg;
    struct Program *prg;

    int8_t crc_validity[NB_PID_MAX];
    /** filters for various streams specified by PMT + for the PAT and PMT */
    MpegTSFilter *pids[NB_PID_MAX];
    int current_pid;

    AVStream *epg_stream;
    AVBufferPool* pools[32];
};

#define MPEGTS_OPTIONS \
    { "resync_size",   "set size limit for looking up a new synchronization",  \
        offsetof(MpegTSContext, resync_size), AV_OPT_TYPE_INT,                 \
        { .i64 =  MAX_RESYNC_SIZE}, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },  \
    { "ts_id", "transport stream id",                                          \
        offsetof(MpegTSContext, id), AV_OPT_TYPE_INT,                          \
        { .i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY }, \
    { "ts_packetsize", "output option carrying the raw packet size",           \
        offsetof(MpegTSContext, raw_packet_size), AV_OPT_TYPE_INT,             \
        { .i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY }

static const AVOption options[] = {
    MPEGTS_OPTIONS,
    {"fix_teletext_pts", "try to fix pts values of dvb teletext streams", offsetof(MpegTSContext, fix_teletext_pts), AV_OPT_TYPE_BOOL,
     {.i64 = 1}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    {"scan_all_pmts", "scan and combine all PMTs", offsetof(MpegTSContext, scan_all_pmts), AV_OPT_TYPE_BOOL,
     {.i64 = -1}, -1, 1, AV_OPT_FLAG_DECODING_PARAM },
    {"skip_unknown_pmt", "skip PMTs for programs not advertised in the PAT", offsetof(MpegTSContext, skip_unknown_pmt), AV_OPT_TYPE_BOOL,
     {.i64 = 0}, 0, 1, AV_OPT_FLAG_DECODING_PARAM },
    {"merge_pmt_versions", "re-use streams when PMT's version/pids change", offsetof(MpegTSContext, merge_pmt_versions), AV_OPT_TYPE_BOOL,
     {.i64 = 0}, 0, 1,  AV_OPT_FLAG_DECODING_PARAM },
    {"skip_changes", "skip changing / adding streams / programs", offsetof(MpegTSContext, skip_changes), AV_OPT_TYPE_BOOL,
     {.i64 = 0}, 0, 1, 0 },
    {"skip_clear", "skip clearing programs", offsetof(MpegTSContext, skip_clear), AV_OPT_TYPE_BOOL,
     {.i64 = 0}, 0, 1, 0 },
    {"max_packet_size", "maximum size of emitted packet", offsetof(MpegTSContext, max_packet_size), AV_OPT_TYPE_INT,
     {.i64 = 204800}, 1, INT_MAX/2, AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass mpegts_class = {
    .class_name = "mpegts demuxer",
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVOption raw_options[] = {
    MPEGTS_OPTIONS,
    { "compute_pcr",   "compute exact PCR for each transport stream packet",
          offsetof(MpegTSContext, mpeg2ts_compute_pcr), AV_OPT_TYPE_BOOL,
          { .i64 = 0 }, 0, 1,  AV_OPT_FLAG_DECODING_PARAM },
    { NULL },
};

static const AVClass mpegtsraw_class = {
    .class_name = "mpegtsraw demuxer",
    .option     = raw_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* TS stream handling */

enum MpegTSState {
    MPEGTS_HEADER = 0,
    MPEGTS_PESHEADER,
    MPEGTS_PESHEADER_FILL,
    MPEGTS_PAYLOAD,
    MPEGTS_SKIP,
};

/* enough for PES header + length */
#define PES_START_SIZE  6
#define PES_HEADER_SIZE 9
#define MAX_PES_HEADER_SIZE (9 + 255)

typedef struct PESContext {
    int pid;
    int pcr_pid; /**< if -1 then all packets containing PCR are considered */
    int stream_type;
    MpegTSContext *ts;
    AVFormatContext *stream;
    AVStream *st;
    AVStream *sub_st; /**< stream for the embedded AC3 stream in HDMV TrueHD */
    enum MpegTSState state;
    /* used to get the format */
    int data_index;
    int flags; /**< copied to the AVPacket flags */
    int PES_packet_length;
    int pes_header_size;
    int extended_stream_id;
    uint8_t stream_id;
    int64_t pts, dts;
    int64_t ts_packet_pos; /**< position of first TS packet of this PES packet */
    uint8_t header[MAX_PES_HEADER_SIZE];
    AVBufferRef *buffer;
    SLConfigDescr sl;
    int merged_st;
} PESContext;

EXTERN const FFInputFormat ff_mpegts_demuxer;

static struct Program * get_program(MpegTSContext *ts, unsigned int programid)
{
    int i;
    for (i = 0; i < ts->nb_prg; i++) {
        if (ts->prg[i].id == programid) {
            return &ts->prg[i];
        }
    }
    return NULL;
}

static void clear_avprogram(MpegTSContext *ts, unsigned int programid)
{
    AVProgram *prg = NULL;
    int i;

    for (i = 0; i < ts->stream->nb_programs; i++)
        if (ts->stream->programs[i]->id == programid) {
            prg = ts->stream->programs[i];
            break;
        }
    if (!prg)
        return;
    prg->nb_stream_indexes = 0;
}

static void clear_program(struct Program *p)
{
    if (!p)
        return;
    p->nb_pids = 0;
    p->nb_streams = 0;
    p->pmt_found = 0;
}

static void clear_programs(MpegTSContext *ts)
{
    av_freep(&ts->prg);
    ts->nb_prg = 0;
}

static struct Program * add_program(MpegTSContext *ts, unsigned int programid)
{
    struct Program *p = get_program(ts, programid);
    if (p)
        return p;
    if (av_reallocp_array(&ts->prg, ts->nb_prg + 1, sizeof(*ts->prg)) < 0) {
        ts->nb_prg = 0;
        return NULL;
    }
    p = &ts->prg[ts->nb_prg];
    p->id = programid;
    clear_program(p);
    ts->nb_prg++;
    return p;
}

static void add_pid_to_program(struct Program *p, unsigned int pid)
{
    int i;
    if (!p)
        return;

    if (p->nb_pids >= MAX_PIDS_PER_PROGRAM)
        return;

    for (i = 0; i < p->nb_pids; i++)
        if (p->pids[i] == pid)
            return;

    p->pids[p->nb_pids++] = pid;
}

static void update_av_program_info(AVFormatContext *s, unsigned int programid,
                                   unsigned int pid, int version)
{
    int i;
    for (i = 0; i < s->nb_programs; i++) {
        AVProgram *program = s->programs[i];
        if (program->id == programid) {
            int old_pcr_pid = program->pcr_pid,
                old_version = program->pmt_version;
            program->pcr_pid = pid;
            program->pmt_version = version;

            if (old_version != -1 && old_version != version) {
                av_log(s, AV_LOG_VERBOSE,
                       "detected PMT change (program=%d, version=%d/%d, pcr_pid=0x%x/0x%x)\n",
                       programid, old_version, version, old_pcr_pid, pid);
            }
            break;
        }
    }
}

/**
 * @brief discard_pid() decides if the pid is to be discarded according
 *                      to caller's programs selection
 * @param ts    : - TS context
 * @param pid   : - pid
 * @return 1 if the pid is only comprised in programs that have .discard=AVDISCARD_ALL
 *         0 otherwise
 */
static int discard_pid(MpegTSContext *ts, unsigned int pid)
{
    int i, j, k;
    int used = 0, discarded = 0;
    struct Program *p;

    if (pid == PAT_PID)
        return 0;

    /* If none of the programs have .discard=AVDISCARD_ALL then there's
     * no way we have to discard this packet */
    for (k = 0; k < ts->stream->nb_programs; k++)
        if (ts->stream->programs[k]->discard == AVDISCARD_ALL)
            break;
    if (k == ts->stream->nb_programs)
        return 0;

    for (i = 0; i < ts->nb_prg; i++) {
        p = &ts->prg[i];
        for (j = 0; j < p->nb_pids; j++) {
            if (p->pids[j] != pid)
                continue;
            // is program with id p->id set to be discarded?
            for (k = 0; k < ts->stream->nb_programs; k++) {
                if (ts->stream->programs[k]->id == p->id) {
                    if (ts->stream->programs[k]->discard == AVDISCARD_ALL)
                        discarded++;
                    else
                        used++;
                }
            }
        }
    }

    return !used && discarded;
}

/**
 *  Assemble PES packets out of TS packets, and then call the "section_cb"
 *  function when they are complete.
 */
static void write_section_data(MpegTSContext *ts, MpegTSFilter *tss1,
                               const uint8_t *buf, int buf_size, int is_start)
{
    MpegTSSectionFilter *tss = &tss1->u.section_filter;
    uint8_t *cur_section_buf = NULL;
    int len, offset;

    if (is_start) {
        memcpy(tss->section_buf, buf, buf_size);
        tss->section_index = buf_size;
        tss->section_h_size = -1;
        tss->end_of_section_reached = 0;
    } else {
        if (tss->end_of_section_reached)
            return;
        len = MAX_SECTION_SIZE - tss->section_index;
        if (buf_size < len)
            len = buf_size;
        memcpy(tss->section_buf + tss->section_index, buf, len);
        tss->section_index += len;
    }

    offset = 0;
    cur_section_buf = tss->section_buf;
    while (cur_section_buf - tss->section_buf < MAX_SECTION_SIZE && cur_section_buf[0] != STUFFING_BYTE) {
        /* compute section length if possible */
        if (tss->section_h_size == -1 && tss->section_index - offset >= 3) {
            len = (AV_RB16(cur_section_buf + 1) & 0xfff) + 3;
            if (len > MAX_SECTION_SIZE)
                return;
            tss->section_h_size = len;
        }

        if (tss->section_h_size != -1 &&
            tss->section_index >= offset + tss->section_h_size) {
            int crc_valid = 1;
            tss->end_of_section_reached = 1;

            if (tss->check_crc) {
                crc_valid = !av_crc(av_crc_get_table(AV_CRC_32_IEEE), -1, cur_section_buf, tss->section_h_size);
                if (tss->section_h_size >= 4)
                    tss->crc = AV_RB32(cur_section_buf + tss->section_h_size - 4);

                if (crc_valid) {
                    ts->crc_validity[ tss1->pid ] = 100;
                }else if (ts->crc_validity[ tss1->pid ] > -10) {
                    ts->crc_validity[ tss1->pid ]--;
                }else
                    crc_valid = 2;
            }
            if (crc_valid) {
                tss->section_cb(tss1, cur_section_buf, tss->section_h_size);
                if (crc_valid != 1)
                    tss->last_ver = -1;
            }

            cur_section_buf += tss->section_h_size;
            offset += tss->section_h_size;
            tss->section_h_size = -1;
        } else {
            tss->section_h_size = -1;
            tss->end_of_section_reached = 0;
            break;
        }
    }
}

static MpegTSFilter *mpegts_open_filter(MpegTSContext *ts, unsigned int pid,
                                        enum MpegTSFilterType type)
{
    MpegTSFilter *filter;

    av_log(ts->stream, AV_LOG_TRACE, "Filter: pid=0x%x type=%d\n", pid, type);

    if (pid >= NB_PID_MAX || ts->pids[pid])
        return NULL;
    filter = av_mallocz(sizeof(MpegTSFilter));
    if (!filter)
        return NULL;
    ts->pids[pid] = filter;

    filter->type    = type;
    filter->pid     = pid;
    filter->es_id   = -1;
    filter->last_cc = -1;
    filter->last_pcr= -1;

    return filter;
}

static MpegTSFilter *mpegts_open_section_filter(MpegTSContext *ts,
                                                unsigned int pid,
                                                SectionCallback *section_cb,
                                                void *opaque,
                                                int check_crc)
{
    MpegTSFilter *filter;
    MpegTSSectionFilter *sec;
    uint8_t *section_buf = av_mallocz(MAX_SECTION_SIZE);

    if (!section_buf)
        return NULL;

    if (!(filter = mpegts_open_filter(ts, pid, MPEGTS_SECTION))) {
        av_free(section_buf);
        return NULL;
    }
    sec = &filter->u.section_filter;
    sec->section_cb  = section_cb;
    sec->opaque      = opaque;
    sec->section_buf = section_buf;
    sec->check_crc   = check_crc;
    sec->last_ver    = -1;

    return filter;
}

static MpegTSFilter *mpegts_open_pes_filter(MpegTSContext *ts, unsigned int pid,
                                            PESCallback *pes_cb,
                                            void *opaque)
{
    MpegTSFilter *filter;
    MpegTSPESFilter *pes;

    if (!(filter = mpegts_open_filter(ts, pid, MPEGTS_PES)))
        return NULL;

    pes = &filter->u.pes_filter;
    pes->pes_cb = pes_cb;
    pes->opaque = opaque;
    return filter;
}

static MpegTSFilter *mpegts_open_pcr_filter(MpegTSContext *ts, unsigned int pid)
{
    return mpegts_open_filter(ts, pid, MPEGTS_PCR);
}

static void mpegts_close_filter(MpegTSContext *ts, MpegTSFilter *filter)
{
    int pid;

    pid = filter->pid;
    if (filter->type == MPEGTS_SECTION)
        av_freep(&filter->u.section_filter.section_buf);
    else if (filter->type == MPEGTS_PES) {
        PESContext *pes = filter->u.pes_filter.opaque;
        av_buffer_unref(&pes->buffer);
        /* referenced private data will be freed later in
         * avformat_close_input (pes->st->priv_data == pes) */
        if (!pes->st || pes->merged_st) {
            av_freep(&filter->u.pes_filter.opaque);
        }
    }

    av_free(filter);
    ts->pids[pid] = NULL;
}

static int analyze(const uint8_t *buf, int size, int packet_size,
                   int probe)
{
    int stat[TS_MAX_PACKET_SIZE];
    int stat_all = 0;
    int i;
    int best_score = 0;

    memset(stat, 0, packet_size * sizeof(*stat));

    for (i = 0; i < size - 3; i++) {
        if (buf[i] == SYNC_BYTE) {
            int pid = AV_RB16(buf+1) & 0x1FFF;
            int asc = buf[i + 3] & 0x30;
            if (!probe || pid == 0x1FFF || asc) {
                int x = i % packet_size;
                stat[x]++;
                stat_all++;
                if (stat[x] > best_score) {
                    best_score = stat[x];
                }
            }
        }
    }

    return best_score - FFMAX(stat_all - 10*best_score, 0)/10;
}

/* autodetect fec presence */
static int get_packet_size(AVFormatContext* s)
{
    int score, fec_score, dvhs_score;
    int margin;
    int ret;

    /*init buffer to store stream for probing */
    uint8_t buf[PROBE_PACKET_MAX_BUF] = {0};
    int buf_size = 0;
    int max_iterations = 16;

    while (buf_size < PROBE_PACKET_MAX_BUF && max_iterations--) {
        ret = avio_read_partial(s->pb, buf + buf_size, PROBE_PACKET_MAX_BUF - buf_size);
        if (ret < 0)
            return AVERROR_INVALIDDATA;
        buf_size += ret;

        score      = analyze(buf, buf_size, TS_PACKET_SIZE,      0);
        dvhs_score = analyze(buf, buf_size, TS_DVHS_PACKET_SIZE, 0);
        fec_score  = analyze(buf, buf_size, TS_FEC_PACKET_SIZE,  0);
        av_log(s, AV_LOG_TRACE, "Probe: %d, score: %d, dvhs_score: %d, fec_score: %d \n",
            buf_size, score, dvhs_score, fec_score);

        margin = mid_pred(score, fec_score, dvhs_score);

        if (buf_size < PROBE_PACKET_MAX_BUF)
            margin += PROBE_PACKET_MARGIN; /*if buffer not filled */

        if (score > margin)
            return TS_PACKET_SIZE;
        else if (dvhs_score > margin)
            return TS_DVHS_PACKET_SIZE;
        else if (fec_score > margin)
            return TS_FEC_PACKET_SIZE;
    }
    return AVERROR_INVALIDDATA;
}

typedef struct SectionHeader {
    uint8_t tid;
    uint16_t id;
    uint8_t version;
    uint8_t current_next;
    uint8_t sec_num;
    uint8_t last_sec_num;
} SectionHeader;

static int skip_identical(const SectionHeader *h, MpegTSSectionFilter *tssf)
{
    if (h->version == tssf->last_ver && tssf->last_crc == tssf->crc)
        return 1;

    tssf->last_ver = h->version;
    tssf->last_crc = tssf->crc;

    return 0;
}

static inline int get8(const uint8_t **pp, const uint8_t *p_end)
{
    const uint8_t *p;
    int c;

    p = *pp;
    if (p >= p_end)
        return AVERROR_INVALIDDATA;
    c   = *p++;
    *pp = p;
    return c;
}

static inline int get16(const uint8_t **pp, const uint8_t *p_end)
{
    const uint8_t *p;
    int c;

    p = *pp;
    if (1 >= p_end - p)
        return AVERROR_INVALIDDATA;
    c   = AV_RB16(p);
    p  += 2;
    *pp = p;
    return c;
}

/* read and allocate a DVB string preceded by its length */
static char *getstr8(const uint8_t **pp, const uint8_t *p_end)
{
    int len;
    const uint8_t *p;
    char *str;

    p   = *pp;
    len = get8(&p, p_end);
    if (len < 0)
        return NULL;
    if (len > p_end - p)
        return NULL;
#if CONFIG_ICONV
    if (len) {
        const char *encodings[] = {
            "ISO6937", "ISO-8859-5", "ISO-8859-6", "ISO-8859-7",
            "ISO-8859-8", "ISO-8859-9", "ISO-8859-10", "ISO-8859-11",
            "", "ISO-8859-13", "ISO-8859-14", "ISO-8859-15", "", "", "", "",
            "", "UCS-2BE", "KSC_5601", "GB2312", "UCS-2BE", "UTF-8", "", "",
            "", "", "", "", "", "", "", ""
        };
        iconv_t cd;
        char *in, *out;
        size_t inlen = len, outlen = inlen * 6 + 1;
        if (len >= 3 && p[0] == 0x10 && !p[1] && p[2] && p[2] <= 0xf && p[2] != 0xc) {
            char iso8859[12];
            snprintf(iso8859, sizeof(iso8859), "ISO-8859-%d", p[2]);
            inlen -= 3;
            in = (char *)p + 3;
            cd = iconv_open("UTF-8", iso8859);
        } else if (p[0] < 0x20) {
            inlen -= 1;
            in = (char *)p + 1;
            cd = iconv_open("UTF-8", encodings[*p]);
        } else {
            in = (char *)p;
            cd = iconv_open("UTF-8", encodings[0]);
        }
        if (cd == (iconv_t)-1)
            goto no_iconv;
        str = out = av_malloc(outlen);
        if (!str) {
            iconv_close(cd);
            return NULL;
        }
        if (iconv(cd, &in, &inlen, &out, &outlen) == -1) {
            iconv_close(cd);
            av_freep(&str);
            goto no_iconv;
        }
        iconv_close(cd);
        *out = 0;
        *pp = p + len;
        return str;
    }
no_iconv:
#endif
    str = av_malloc(len + 1);
    if (!str)
        return NULL;
    memcpy(str, p, len);
    str[len] = '\0';
    p  += len;
    *pp = p;
    return str;
}

static int parse_section_header(SectionHeader *h,
                                const uint8_t **pp, const uint8_t *p_end)
{
    int val;

    val = get8(pp, p_end);
    if (val < 0)
        return val;
    h->tid = val;
    *pp += 2;
    val  = get16(pp, p_end);
    if (val < 0)
        return val;
    h->id = val;
    val = get8(pp, p_end);
    if (val < 0)
        return val;
    h->version = (val >> 1) & 0x1f;
    h->current_next = val & 0x01;
    val = get8(pp, p_end);
    if (val < 0)
        return val;
    h->sec_num = val;
    val = get8(pp, p_end);
    if (val < 0)
        return val;
    h->last_sec_num = val;
    return 0;
}

typedef struct StreamType {
    uint32_t stream_type;
    enum AVMediaType codec_type;
    enum AVCodecID codec_id;
} StreamType;

static const StreamType ISO_types[] = {
    { STREAM_TYPE_VIDEO_MPEG1,    AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO },
    { STREAM_TYPE_VIDEO_MPEG2,    AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG2VIDEO },
    { STREAM_TYPE_AUDIO_MPEG1,    AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_MP3        },
    { STREAM_TYPE_AUDIO_MPEG2,    AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_MP3        },
    { STREAM_TYPE_AUDIO_AAC,      AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC        },
    { STREAM_TYPE_VIDEO_MPEG4,    AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG4      },
    /* Makito encoder sets stream type 0x11 for AAC,
     * so auto-detect LOAS/LATM instead of hardcoding it. */
#if !CONFIG_LOAS_DEMUXER
    { STREAM_TYPE_AUDIO_AAC_LATM, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC_LATM   }, /* LATM syntax */
#endif
    { STREAM_TYPE_VIDEO_H264,     AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264       },
    { STREAM_TYPE_AUDIO_MPEG4,    AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC        },
    { STREAM_TYPE_VIDEO_MVC,      AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264       },
    { STREAM_TYPE_VIDEO_JPEG2000, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_JPEG2000   },
    { STREAM_TYPE_VIDEO_HEVC,     AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_HEVC       },
    { STREAM_TYPE_VIDEO_VVC,      AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VVC        },
    { STREAM_TYPE_VIDEO_CAVS,     AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_CAVS       },
    { STREAM_TYPE_VIDEO_DIRAC,    AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_DIRAC      },
    { STREAM_TYPE_VIDEO_AVS2,     AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_AVS2       },
    { STREAM_TYPE_VIDEO_AVS3,     AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_AVS3       },
    { STREAM_TYPE_VIDEO_VC1,      AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VC1        },
    { 0 },
};

static const StreamType HDMV_types[] = {
    { STREAM_TYPE_BLURAY_AUDIO_PCM_BLURAY,            AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_PCM_BLURAY         },
    { STREAM_TYPE_BLURAY_AUDIO_AC3,                   AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_AC3                },
    { STREAM_TYPE_BLURAY_AUDIO_DTS,                   AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_DTS                },
    { STREAM_TYPE_BLURAY_AUDIO_TRUEHD,                AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_TRUEHD             },
    { STREAM_TYPE_BLURAY_AUDIO_EAC3,                  AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_EAC3               },
    { STREAM_TYPE_BLURAY_AUDIO_DTS_HD,                AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_DTS                },
    { STREAM_TYPE_BLURAY_AUDIO_DTS_HD_MASTER,         AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_DTS                },
    { STREAM_TYPE_BLURAY_AUDIO_EAC3_SECONDARY,        AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_EAC3               },
    { STREAM_TYPE_BLURAY_AUDIO_DTS_EXPRESS_SECONDARY, AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_DTS                },
    { STREAM_TYPE_BLURAY_SUBTITLE_PGS,                AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_HDMV_PGS_SUBTITLE  },
    { STREAM_TYPE_BLURAY_SUBTITLE_TEXT,               AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_HDMV_TEXT_SUBTITLE },
    { 0 },
};

/* SCTE types */
static const StreamType SCTE_types[] = {
    { STREAM_TYPE_SCTE_DATA_SCTE_35, AVMEDIA_TYPE_DATA,  AV_CODEC_ID_SCTE_35    },
    { 0 },
};

/* ATSC ? */
static const StreamType MISC_types[] = {
    { STREAM_TYPE_ATSC_AUDIO_AC3,   AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AC3 },
    { STREAM_TYPE_ATSC_AUDIO_EAC3,  AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_EAC3 },
    { 0x8a, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS },
    { 0 },
};

/* HLS Sample Encryption Types  */
static const StreamType HLS_SAMPLE_ENC_types[] = {
    { STREAM_TYPE_HLS_SE_VIDEO_H264, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264},
    { STREAM_TYPE_HLS_SE_AUDIO_AAC,  AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC },
    { STREAM_TYPE_HLS_SE_AUDIO_AC3,  AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AC3 },
    { STREAM_TYPE_HLS_SE_AUDIO_EAC3, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_EAC3},
    { 0 },
};

static const StreamType REGD_types[] = {
    { MKTAG('d', 'r', 'a', 'c'), AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_DIRAC },
    { MKTAG('A', 'C', '-', '3'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AC3   },
    { MKTAG('A', 'C', '-', '4'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AC4   },
    { MKTAG('B', 'S', 'S', 'D'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_S302M },
    { MKTAG('D', 'T', 'S', '1'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS   },
    { MKTAG('D', 'T', 'S', '2'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS   },
    { MKTAG('D', 'T', 'S', '3'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_DTS   },
    { MKTAG('E', 'A', 'C', '3'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_EAC3  },
    { MKTAG('H', 'E', 'V', 'C'), AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_HEVC  },
    { MKTAG('V', 'V', 'C', ' '), AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VVC   },
    { MKTAG('K', 'L', 'V', 'A'), AVMEDIA_TYPE_DATA,  AV_CODEC_ID_SMPTE_KLV },
    { MKTAG('V', 'A', 'N', 'C'), AVMEDIA_TYPE_DATA,  AV_CODEC_ID_SMPTE_2038 },
    { MKTAG('I', 'D', '3', ' '), AVMEDIA_TYPE_DATA,  AV_CODEC_ID_TIMED_ID3 },
    { MKTAG('V', 'C', '-', '1'), AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VC1   },
    { MKTAG('O', 'p', 'u', 's'), AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_OPUS  },
    { 0 },
};

static const StreamType METADATA_types[] = {
    { MKTAG('K','L','V','A'), AVMEDIA_TYPE_DATA, AV_CODEC_ID_SMPTE_KLV },
    { MKTAG('I','D','3',' '), AVMEDIA_TYPE_DATA, AV_CODEC_ID_TIMED_ID3 },
    { 0 },
};

/* descriptor present */
static const StreamType DESC_types[] = {
    { AC3_DESCRIPTOR,           AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_AC3          },
    { ENHANCED_AC3_DESCRIPTOR,  AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_EAC3         },
    { DTS_DESCRIPTOR,           AVMEDIA_TYPE_AUDIO,    AV_CODEC_ID_DTS          },
    { TELETEXT_DESCRIPTOR,      AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_DVB_TELETEXT },
    { SUBTITLING_DESCRIPTOR,    AVMEDIA_TYPE_SUBTITLE, AV_CODEC_ID_DVB_SUBTITLE },
    { 0 },
};

static void mpegts_find_stream_type(AVStream *st,
                                    uint32_t stream_type,
                                    const StreamType *types)
{
    FFStream *const sti = ffstream(st);
    for (; types->stream_type; types++)
        if (stream_type == types->stream_type) {
            if (st->codecpar->codec_type != types->codec_type ||
                st->codecpar->codec_id   != types->codec_id) {
                st->codecpar->codec_type = types->codec_type;
                st->codecpar->codec_id   = types->codec_id;
                sti->need_context_update = 1;
            }
            sti->request_probe = 0;
            return;
        }
}

static int mpegts_set_stream_info(AVStream *st, PESContext *pes,
                                  uint32_t stream_type, uint32_t prog_reg_desc)
{
    FFStream *const sti = ffstream(st);
    int old_codec_type = st->codecpar->codec_type;
    int old_codec_id   = st->codecpar->codec_id;
    int old_codec_tag  = st->codecpar->codec_tag;

    avpriv_set_pts_info(st, 33, 1, 90000);
    st->priv_data         = pes;
    st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
    st->codecpar->codec_id   = AV_CODEC_ID_NONE;
    sti->need_parsing = AVSTREAM_PARSE_FULL;
    pes->st          = st;
    pes->stream_type = stream_type;

    av_log(pes->stream, AV_LOG_DEBUG,
           "stream=%d stream_type=%x pid=%x prog_reg_desc=%.4s\n",
           st->index, pes->stream_type, pes->pid, (char *)&prog_reg_desc);

    st->codecpar->codec_tag = pes->stream_type;

    mpegts_find_stream_type(st, pes->stream_type, ISO_types);
    if (pes->stream_type == STREAM_TYPE_AUDIO_MPEG2 || pes->stream_type == STREAM_TYPE_AUDIO_AAC)
        sti->request_probe = 50;
    if (pes->stream_type == STREAM_TYPE_PRIVATE_DATA)
        sti->request_probe = AVPROBE_SCORE_STREAM_RETRY;
    if ((prog_reg_desc == AV_RL32("HDMV") ||
         prog_reg_desc == AV_RL32("HDPR")) &&
        st->codecpar->codec_id == AV_CODEC_ID_NONE) {
        mpegts_find_stream_type(st, pes->stream_type, HDMV_types);
        if (pes->stream_type == STREAM_TYPE_BLURAY_AUDIO_TRUEHD) {
            // HDMV TrueHD streams also contain an AC3 coded version of the
            // audio track - add a second stream for this
            AVStream *sub_st;
            // priv_data cannot be shared between streams
            PESContext *sub_pes = av_memdup(pes, sizeof(*sub_pes));
            if (!sub_pes)
                return AVERROR(ENOMEM);

            sub_st = avformat_new_stream(pes->stream, NULL);
            if (!sub_st) {
                av_free(sub_pes);
                return AVERROR(ENOMEM);
            }

            sub_st->id = pes->pid;
            avpriv_set_pts_info(sub_st, 33, 1, 90000);
            sub_st->priv_data         = sub_pes;
            sub_st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            sub_st->codecpar->codec_id   = AV_CODEC_ID_AC3;
            ffstream(sub_st)->need_parsing = AVSTREAM_PARSE_FULL;
            sub_pes->sub_st           = pes->sub_st = sub_st;
        }
    }
    if (st->codecpar->codec_id == AV_CODEC_ID_NONE)
        mpegts_find_stream_type(st, pes->stream_type, MISC_types);
    if (st->codecpar->codec_id == AV_CODEC_ID_NONE)
        mpegts_find_stream_type(st, pes->stream_type, HLS_SAMPLE_ENC_types);
    if (st->codecpar->codec_id == AV_CODEC_ID_NONE) {
        st->codecpar->codec_id  = old_codec_id;
        st->codecpar->codec_type = old_codec_type;
    }
    if ((st->codecpar->codec_id == AV_CODEC_ID_NONE ||
            (sti->request_probe > 0 && sti->request_probe < AVPROBE_SCORE_STREAM_RETRY / 5)) &&
        sti->probe_packets > 0 &&
        stream_type == STREAM_TYPE_PRIVATE_DATA) {
        st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        st->codecpar->codec_id   = AV_CODEC_ID_BIN_DATA;
        sti->request_probe = AVPROBE_SCORE_STREAM_RETRY / 5;
    }

    /* queue a context update if properties changed */
    if (old_codec_type != st->codecpar->codec_type ||
        old_codec_id   != st->codecpar->codec_id   ||
        old_codec_tag  != st->codecpar->codec_tag)
        sti->need_context_update = 1;

    return 0;
}

static void reset_pes_packet_state(PESContext *pes)
{
    pes->pts        = AV_NOPTS_VALUE;
    pes->dts        = AV_NOPTS_VALUE;
    pes->data_index = 0;
    pes->flags      = 0;
    av_buffer_unref(&pes->buffer);
}

static void new_data_packet(const uint8_t *buffer, int len, AVPacket *pkt)
{
    av_packet_unref(pkt);
    pkt->data = (uint8_t *)buffer;
    pkt->size = len;
}

static int new_pes_packet(PESContext *pes, AVPacket *pkt)
{
    uint8_t *sd;

    av_packet_unref(pkt);

    pkt->buf  = pes->buffer;
    pkt->data = pes->buffer->data;
    pkt->size = pes->data_index;

    if (pes->PES_packet_length &&
        pes->pes_header_size + pes->data_index != pes->PES_packet_length +
        PES_START_SIZE) {
        av_log(pes->stream, AV_LOG_WARNING, "PES packet size mismatch\n");
        pes->flags |= AV_PKT_FLAG_CORRUPT;
    }
    memset(pkt->data + pkt->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    // Separate out the AC3 substream from an HDMV combined TrueHD/AC3 PID
    if (pes->sub_st && pes->stream_type == STREAM_TYPE_BLURAY_AUDIO_TRUEHD && pes->extended_stream_id == 0x76)
        pkt->stream_index = pes->sub_st->index;
    else
        pkt->stream_index = pes->st->index;
    pkt->pts = pes->pts;
    pkt->dts = pes->dts;
    /* store position of first TS packet of this PES packet */
    pkt->pos   = pes->ts_packet_pos;
    pkt->flags = pes->flags;

    pes->buffer = NULL;
    reset_pes_packet_state(pes);

    sd = av_packet_new_side_data(pkt, AV_PKT_DATA_MPEGTS_STREAM_ID, 1);
    if (!sd)
        return AVERROR(ENOMEM);
    *sd = pes->stream_id;

    return 0;
}

static uint64_t get_ts64(GetBitContext *gb, int bits)
{
    if (get_bits_left(gb) < bits)
        return AV_NOPTS_VALUE;
    return get_bits64(gb, bits);
}

static int read_sl_header(PESContext *pes, SLConfigDescr *sl,
                          const uint8_t *buf, int buf_size)
{
    GetBitContext gb;
    int au_start_flag = 0, au_end_flag = 0, ocr_flag = 0, idle_flag = 0;
    int padding_flag = 0, padding_bits = 0, inst_bitrate_flag = 0;
    int dts_flag = -1, cts_flag = -1;
    int64_t dts = AV_NOPTS_VALUE, cts = AV_NOPTS_VALUE;
    uint8_t buf_padded[128 + AV_INPUT_BUFFER_PADDING_SIZE];
    int buf_padded_size = FFMIN(buf_size, sizeof(buf_padded) - AV_INPUT_BUFFER_PADDING_SIZE);

    memcpy(buf_padded, buf, buf_padded_size);

    init_get_bits(&gb, buf_padded, buf_padded_size * 8);

    if (sl->use_au_start)
        au_start_flag = get_bits1(&gb);
    if (sl->use_au_end)
        au_end_flag = get_bits1(&gb);
    if (!sl->use_au_start && !sl->use_au_end)
        au_start_flag = au_end_flag = 1;
    if (sl->ocr_len > 0)
        ocr_flag = get_bits1(&gb);
    if (sl->use_idle)
        idle_flag = get_bits1(&gb);
    if (sl->use_padding)
        padding_flag = get_bits1(&gb);
    if (padding_flag)
        padding_bits = get_bits(&gb, 3);

    if (!idle_flag && (!padding_flag || padding_bits != 0)) {
        if (sl->packet_seq_num_len)
            skip_bits_long(&gb, sl->packet_seq_num_len);
        if (sl->degr_prior_len)
            if (get_bits1(&gb))
                skip_bits(&gb, sl->degr_prior_len);
        if (ocr_flag)
            skip_bits_long(&gb, sl->ocr_len);
        if (au_start_flag) {
            if (sl->use_rand_acc_pt)
                get_bits1(&gb);
            if (sl->au_seq_num_len > 0)
                skip_bits_long(&gb, sl->au_seq_num_len);
            if (sl->use_timestamps) {
                dts_flag = get_bits1(&gb);
                cts_flag = get_bits1(&gb);
            }
        }
        if (sl->inst_bitrate_len)
            inst_bitrate_flag = get_bits1(&gb);
        if (dts_flag == 1)
            dts = get_ts64(&gb, sl->timestamp_len);
        if (cts_flag == 1)
            cts = get_ts64(&gb, sl->timestamp_len);
        if (sl->au_len > 0)
            skip_bits_long(&gb, sl->au_len);
        if (inst_bitrate_flag)
            skip_bits_long(&gb, sl->inst_bitrate_len);
    }

    if (dts != AV_NOPTS_VALUE)
        pes->dts = dts;
    if (cts != AV_NOPTS_VALUE)
        pes->pts = cts;

    if (sl->timestamp_len && sl->timestamp_res)
        avpriv_set_pts_info(pes->st, sl->timestamp_len, 1, sl->timestamp_res);

    return (get_bits_count(&gb) + 7) >> 3;
}

static AVBufferRef *buffer_pool_get(MpegTSContext *ts, int size)
{
    int index = av_log2(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ts->pools[index]) {
        int pool_size = FFMIN(ts->max_packet_size + AV_INPUT_BUFFER_PADDING_SIZE, 2 << index);
        ts->pools[index] = av_buffer_pool_init(pool_size, NULL);
        if (!ts->pools[index])
            return NULL;
    }
    return av_buffer_pool_get(ts->pools[index]);
}

/* return non zero if a packet could be constructed */
static int mpegts_push_data(MpegTSFilter *filter,
                            const uint8_t *buf, int buf_size, int is_start,
                            int64_t pos)
{
    PESContext *pes   = filter->u.pes_filter.opaque;
    MpegTSContext *ts = pes->ts;
    const uint8_t *p;
    int ret, len;

    if (!ts->pkt)
        return 0;

    if (is_start) {
        if (pes->state == MPEGTS_PAYLOAD && pes->data_index > 0) {
            ret = new_pes_packet(pes, ts->pkt);
            if (ret < 0)
                return ret;
            ts->stop_parse = 1;
        } else {
            reset_pes_packet_state(pes);
        }
        pes->state         = MPEGTS_HEADER;
        pes->ts_packet_pos = pos;
    }
    p = buf;
    while (buf_size > 0) {
        switch (pes->state) {
        case MPEGTS_HEADER:
            len = PES_START_SIZE - pes->data_index;
            if (len > buf_size)
                len = buf_size;
            memcpy(pes->header + pes->data_index, p, len);
            pes->data_index += len;
            p += len;
            buf_size -= len;
            if (pes->data_index == PES_START_SIZE) {
                /* we got all the PES or section header. We can now
                 * decide */
                if (pes->header[0] == 0x00 && pes->header[1] == 0x00 &&
                    pes->header[2] == 0x01) {
                    /* it must be an MPEG-2 PES stream */
                    pes->stream_id = pes->header[3];
                    av_log(pes->stream, AV_LOG_TRACE, "pid=%x stream_id=%#x\n", pes->pid, pes->stream_id);

                    if ((pes->st && pes->st->discard == AVDISCARD_ALL &&
                         (!pes->sub_st ||
                          pes->sub_st->discard == AVDISCARD_ALL)) ||
                        pes->stream_id == STREAM_ID_PADDING_STREAM)
                        goto skip;

                    /* stream not present in PMT */
                    if (!pes->st) {
                        if (ts->skip_changes)
                            goto skip;
                        if (ts->merge_pmt_versions)
                            goto skip; /* wait for PMT to merge new stream */

                        pes->st = avformat_new_stream(ts->stream, NULL);
                        if (!pes->st)
                            return AVERROR(ENOMEM);
                        pes->st->id = pes->pid;
                        mpegts_set_stream_info(pes->st, pes, 0, 0);
                    }

                    pes->PES_packet_length = AV_RB16(pes->header + 4);
                    /* NOTE: zero length means the PES size is unbounded */

                    if (pes->stream_id != STREAM_ID_PROGRAM_STREAM_MAP &&
                        pes->stream_id != STREAM_ID_PRIVATE_STREAM_2 &&
                        pes->stream_id != STREAM_ID_ECM_STREAM &&
                        pes->stream_id != STREAM_ID_EMM_STREAM &&
                        pes->stream_id != STREAM_ID_PROGRAM_STREAM_DIRECTORY &&
                        pes->stream_id != STREAM_ID_DSMCC_STREAM &&
                        pes->stream_id != STREAM_ID_TYPE_E_STREAM) {
                        FFStream *const pes_sti = ffstream(pes->st);
                        pes->state = MPEGTS_PESHEADER;
                        if (pes->st->codecpar->codec_id == AV_CODEC_ID_NONE && !pes_sti->request_probe) {
                            av_log(pes->stream, AV_LOG_TRACE,
                                    "pid=%x stream_type=%x probing\n",
                                    pes->pid,
                                    pes->stream_type);
                            pes_sti->request_probe = 1;
                        }
                    } else {
                        pes->pes_header_size = 6;
                        pes->state      = MPEGTS_PAYLOAD;
                        pes->data_index = 0;
                    }
                } else {
                    /* otherwise, it should be a table */
                    /* skip packet */
skip:
                    pes->state = MPEGTS_SKIP;
                    continue;
                }
            }
            break;
        /**********************************************/
        /* PES packing parsing */
        case MPEGTS_PESHEADER:
            len = PES_HEADER_SIZE - pes->data_index;
            if (len < 0)
                return AVERROR_INVALIDDATA;
            if (len > buf_size)
                len = buf_size;
            memcpy(pes->header + pes->data_index, p, len);
            pes->data_index += len;
            p += len;
            buf_size -= len;
            if (pes->data_index == PES_HEADER_SIZE) {
                pes->pes_header_size = pes->header[8] + 9;
                pes->state           = MPEGTS_PESHEADER_FILL;
            }
            break;
        case MPEGTS_PESHEADER_FILL:
            len = pes->pes_header_size - pes->data_index;
            if (len < 0)
                return AVERROR_INVALIDDATA;
            if (len > buf_size)
                len = buf_size;
            memcpy(pes->header + pes->data_index, p, len);
            pes->data_index += len;
            p += len;
            buf_size -= len;
            if (pes->data_index == pes->pes_header_size) {
                const uint8_t *r;
                unsigned int flags, pes_ext, skip;

                flags = pes->header[7];
                r = pes->header + 9;
                pes->pts = AV_NOPTS_VALUE;
                pes->dts = AV_NOPTS_VALUE;
                if ((flags & 0xc0) == 0x80) {
                    pes->dts = pes->pts = ff_parse_pes_pts(r);
                    r += 5;
                } else if ((flags & 0xc0) == 0xc0) {
                    pes->pts = ff_parse_pes_pts(r);
                    r += 5;
                    pes->dts = ff_parse_pes_pts(r);
                    r += 5;
                }
                pes->extended_stream_id = -1;
                if (flags & 0x01) { /* PES extension */
                    pes_ext = *r++;
                    /* Skip PES private data, program packet sequence counter and P-STD buffer */
                    skip  = (pes_ext >> 4) & 0xb;
                    skip += skip & 0x9;
                    r    += skip;
                    if ((pes_ext & 0x41) == 0x01 &&
                        (r + 2) <= (pes->header + pes->pes_header_size)) {
                        /* PES extension 2 */
                        if ((r[0] & 0x7f) > 0 && (r[1] & 0x80) == 0)
                            pes->extended_stream_id = r[1];
                    }
                }

                /* we got the full header. We parse it and get the payload */
                pes->state = MPEGTS_PAYLOAD;
                pes->data_index = 0;
                if (pes->stream_type == STREAM_TYPE_ISO_IEC_14496_PES && buf_size > 0) {
                    int sl_header_bytes = read_sl_header(pes, &pes->sl, p,
                                                         buf_size);
                    pes->pes_header_size += sl_header_bytes;
                    p += sl_header_bytes;
                    buf_size -= sl_header_bytes;
                }
                if (pes->stream_type == STREAM_TYPE_METADATA &&
                    pes->stream_id == STREAM_ID_METADATA_STREAM &&
                    pes->st->codecpar->codec_id == AV_CODEC_ID_SMPTE_KLV &&
                    buf_size >= 5) {
                    /* skip metadata access unit header - see MISB ST 1402 */
                    pes->pes_header_size += 5;
                    p += 5;
                    buf_size -= 5;
                }
                if (   pes->ts->fix_teletext_pts
                    && (   pes->st->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT
                        || pes->st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE)
                    ) {
                    AVProgram *p = NULL;
                    int pcr_found = 0;
                    while ((p = av_find_program_from_stream(pes->stream, p, pes->st->index))) {
                        if (p->pcr_pid != -1 && p->discard != AVDISCARD_ALL) {
                            MpegTSFilter *f = pes->ts->pids[p->pcr_pid];
                            if (f) {
                                AVStream *st = NULL;
                                if (f->type == MPEGTS_PES) {
                                    PESContext *pcrpes = f->u.pes_filter.opaque;
                                    if (pcrpes)
                                        st = pcrpes->st;
                                } else if (f->type == MPEGTS_PCR) {
                                    int i;
                                    for (i = 0; i < p->nb_stream_indexes; i++) {
                                        AVStream *pst = pes->stream->streams[p->stream_index[i]];
                                        if (pst->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                                            st = pst;
                                    }
                                }
                                if (f->last_pcr != -1 && !f->discard) {
                                    // teletext packets do not always have correct timestamps,
                                    // the standard says they should be handled after 40.6 ms at most,
                                    // and the pcr error to this packet should be no more than 100 ms.
                                    // TODO: we should interpolate the PCR, not just use the last one
                                    int64_t pcr = f->last_pcr / SYSTEM_CLOCK_FREQUENCY_DIVISOR;
                                    pcr_found = 1;
                                    if (st) {
                                        const FFStream *const sti = ffstream(st);
                                        FFStream *const pes_sti   = ffstream(pes->st);

                                        pes_sti->pts_wrap_reference = sti->pts_wrap_reference;
                                        pes_sti->pts_wrap_behavior  = sti->pts_wrap_behavior;
                                    }
                                    if (pes->dts == AV_NOPTS_VALUE || pes->dts < pcr) {
                                        pes->pts = pes->dts = pcr;
                                    } else if (pes->st->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT &&
                                               pes->dts > pcr + 3654 + 9000) {
                                        pes->pts = pes->dts = pcr + 3654 + 9000;
                                    } else if (pes->st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE &&
                                               pes->dts > pcr + 10*90000) { //10sec
                                        pes->pts = pes->dts = pcr + 3654 + 9000;
                                    }
                                    break;
                                }
                            }
                        }
                    }

                    if (pes->st->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT &&
                        !pcr_found) {
                        av_log(pes->stream, AV_LOG_VERBOSE,
                               "Forcing DTS/PTS to be unset for a "
                               "non-trustworthy PES packet for PID %d as "
                               "PCR hasn't been received yet.\n",
                               pes->pid);
                        pes->dts = pes->pts = AV_NOPTS_VALUE;
                    }
                }
            }
            break;
        case MPEGTS_PAYLOAD:
            do {
                int max_packet_size = ts->max_packet_size;
                if (pes->PES_packet_length && pes->PES_packet_length + PES_START_SIZE > pes->pes_header_size)
                    max_packet_size = pes->PES_packet_length + PES_START_SIZE - pes->pes_header_size;

                if (pes->data_index > 0 &&
                    pes->data_index + buf_size > max_packet_size) {
                    ret = new_pes_packet(pes, ts->pkt);
                    if (ret < 0)
                        return ret;
                    pes->PES_packet_length = 0;
                    max_packet_size = ts->max_packet_size;
                    ts->stop_parse = 1;
                } else if (pes->data_index == 0 &&
                           buf_size > max_packet_size) {
                    // pes packet size is < ts size packet and pes data is padded with STUFFING_BYTE
                    // not sure if this is legal in ts but see issue #2392
                    buf_size = max_packet_size;
                }

                if (!pes->buffer) {
                    pes->buffer = buffer_pool_get(ts, max_packet_size);
                    if (!pes->buffer)
                        return AVERROR(ENOMEM);
                }

                memcpy(pes->buffer->data + pes->data_index, p, buf_size);
                pes->data_index += buf_size;
                /* emit complete packets with known packet size
                 * decreases demuxer delay for infrequent packets like subtitles from
                 * a couple of seconds to milliseconds for properly muxed files. */
                if (!ts->stop_parse && pes->PES_packet_length &&
                    pes->pes_header_size + pes->data_index == pes->PES_packet_length + PES_START_SIZE) {
                    ts->stop_parse = 1;
                    ret = new_pes_packet(pes, ts->pkt);
                    pes->state = MPEGTS_SKIP;
                    if (ret < 0)
                        return ret;
                }
            } while (0);
            buf_size = 0;
            break;
        case MPEGTS_SKIP:
            buf_size = 0;
            break;
        }
    }

    return 0;
}

static PESContext *add_pes_stream(MpegTSContext *ts, int pid, int pcr_pid)
{
    MpegTSFilter *tss;
    PESContext *pes;

    /* if no pid found, then add a pid context */
    pes = av_mallocz(sizeof(PESContext));
    if (!pes)
        return 0;
    pes->ts      = ts;
    pes->stream  = ts->stream;
    pes->pid     = pid;
    pes->pcr_pid = pcr_pid;
    pes->state   = MPEGTS_SKIP;
    pes->pts     = AV_NOPTS_VALUE;
    pes->dts     = AV_NOPTS_VALUE;
    tss          = mpegts_open_pes_filter(ts, pid, mpegts_push_data, pes);
    if (!tss) {
        av_free(pes);
        return 0;
    }
    return pes;
}

#define MAX_LEVEL 4
typedef struct MP4DescrParseContext {
    AVFormatContext *s;
    FFIOContext pb;
    Mp4Descr *descr;
    Mp4Descr *active_descr;
    int descr_count;
    int max_descr_count;
    int level;
    int predefined_SLConfigDescriptor_seen;
} MP4DescrParseContext;

static int init_MP4DescrParseContext(MP4DescrParseContext *d, AVFormatContext *s,
                                     const uint8_t *buf, unsigned size,
                                     Mp4Descr *descr, int max_descr_count)
{
    if (size > (1 << 30))
        return AVERROR_INVALIDDATA;

    ffio_init_read_context(&d->pb, buf, size);

    d->s               = s;
    d->level           = 0;
    d->descr_count     = 0;
    d->descr           = descr;
    d->active_descr    = NULL;
    d->max_descr_count = max_descr_count;

    return 0;
}

static void update_offsets(AVIOContext *pb, int64_t *off, int *len)
{
    int64_t new_off = avio_tell(pb);
    (*len) -= new_off - *off;
    *off    = new_off;
}

static int parse_mp4_descr(MP4DescrParseContext *d, int64_t off, int len,
                           int target_tag);

static int parse_mp4_descr_arr(MP4DescrParseContext *d, int64_t off, int len)
{
    while (len > 0) {
        int ret = parse_mp4_descr(d, off, len, 0);
        if (ret < 0)
            return ret;
        update_offsets(&d->pb.pub, &off, &len);
    }
    return 0;
}

static int parse_MP4IODescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    AVIOContext *const pb = &d->pb.pub;
    avio_rb16(pb); // ID
    avio_r8(pb);
    avio_r8(pb);
    avio_r8(pb);
    avio_r8(pb);
    avio_r8(pb);
    update_offsets(pb, &off, &len);
    return parse_mp4_descr_arr(d, off, len);
}

static int parse_MP4ODescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    int id_flags;
    if (len < 2)
        return 0;
    id_flags = avio_rb16(&d->pb.pub);
    if (!(id_flags & 0x0020)) { // URL_Flag
        update_offsets(&d->pb.pub, &off, &len);
        return parse_mp4_descr_arr(d, off, len); // ES_Descriptor[]
    } else {
        return 0;
    }
}

static int parse_MP4ESDescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    AVIOContext *const pb = &d->pb.pub;
    int es_id = 0;
    int ret   = 0;

    if (d->descr_count >= d->max_descr_count)
        return AVERROR_INVALIDDATA;
    ff_mp4_parse_es_descr(pb, &es_id);
    d->active_descr = d->descr + (d->descr_count++);

    d->active_descr->es_id = es_id;
    update_offsets(pb, &off, &len);
    if ((ret = parse_mp4_descr(d, off, len, MP4DecConfigDescrTag)) < 0)
        return ret;
    update_offsets(pb, &off, &len);
    if (len > 0)
        ret = parse_mp4_descr(d, off, len, MP4SLDescrTag);
    d->active_descr = NULL;
    return ret;
}

static int parse_MP4DecConfigDescrTag(MP4DescrParseContext *d, int64_t off,
                                      int len)
{
    Mp4Descr *descr = d->active_descr;
    if (!descr)
        return AVERROR_INVALIDDATA;
    d->active_descr->dec_config_descr = av_malloc(len);
    if (!descr->dec_config_descr)
        return AVERROR(ENOMEM);
    descr->dec_config_descr_len = len;
    avio_read(&d->pb.pub, descr->dec_config_descr, len);
    return 0;
}

static int parse_MP4SLDescrTag(MP4DescrParseContext *d, int64_t off, int len)
{
    Mp4Descr *descr = d->active_descr;
    AVIOContext *const pb = &d->pb.pub;
    int predefined;
    if (!descr)
        return AVERROR_INVALIDDATA;

#define R8_CHECK_CLIP_MAX(dst, maxv) do {                       \
    descr->sl.dst = avio_r8(pb);                                \
    if (descr->sl.dst > maxv) {                                 \
        descr->sl.dst = maxv;                                   \
        return AVERROR_INVALIDDATA;                             \
    }                                                           \
} while (0)

    predefined = avio_r8(pb);
    if (!predefined) {
        int lengths;
        int flags = avio_r8(pb);
        descr->sl.use_au_start    = !!(flags & 0x80);
        descr->sl.use_au_end      = !!(flags & 0x40);
        descr->sl.use_rand_acc_pt = !!(flags & 0x20);
        descr->sl.use_padding     = !!(flags & 0x08);
        descr->sl.use_timestamps  = !!(flags & 0x04);
        descr->sl.use_idle        = !!(flags & 0x02);
        descr->sl.timestamp_res   = avio_rb32(pb);
        avio_rb32(pb);
        R8_CHECK_CLIP_MAX(timestamp_len, 63);
        R8_CHECK_CLIP_MAX(ocr_len,       63);
        R8_CHECK_CLIP_MAX(au_len,        31);
        descr->sl.inst_bitrate_len   = avio_r8(pb);
        lengths                      = avio_rb16(pb);
        descr->sl.degr_prior_len     = lengths >> 12;
        descr->sl.au_seq_num_len     = (lengths >> 7) & 0x1f;
        descr->sl.packet_seq_num_len = (lengths >> 2) & 0x1f;
    } else if (!d->predefined_SLConfigDescriptor_seen){
        avpriv_report_missing_feature(d->s, "Predefined SLConfigDescriptor");
        d->predefined_SLConfigDescriptor_seen = 1;
    }
    return 0;
}

static int parse_mp4_descr(MP4DescrParseContext *d, int64_t off, int len,
                           int target_tag)
{
    int tag;
    AVIOContext *const pb = &d->pb.pub;
    int len1 = ff_mp4_read_descr(d->s, pb, &tag);
    int ret = 0;

    update_offsets(pb, &off, &len);
    if (len < 0 || len1 > len || len1 <= 0) {
        av_log(d->s, AV_LOG_ERROR,
               "Tag %x length violation new length %d bytes remaining %d\n",
               tag, len1, len);
        return AVERROR_INVALIDDATA;
    }

    if (d->level++ >= MAX_LEVEL) {
        av_log(d->s, AV_LOG_ERROR, "Maximum MP4 descriptor level exceeded\n");
        ret = AVERROR_INVALIDDATA;
        goto done;
    }

    if (target_tag && tag != target_tag) {
        av_log(d->s, AV_LOG_ERROR, "Found tag %x expected %x\n", tag,
               target_tag);
        ret = AVERROR_INVALIDDATA;
        goto done;
    }

    switch (tag) {
    case MP4IODescrTag:
        ret = parse_MP4IODescrTag(d, off, len1);
        break;
    case MP4ODescrTag:
        ret = parse_MP4ODescrTag(d, off, len1);
        break;
    case MP4ESDescrTag:
        ret = parse_MP4ESDescrTag(d, off, len1);
        break;
    case MP4DecConfigDescrTag:
        ret = parse_MP4DecConfigDescrTag(d, off, len1);
        break;
    case MP4SLDescrTag:
        ret = parse_MP4SLDescrTag(d, off, len1);
        break;
    }


done:
    d->level--;
    avio_seek(pb, off + len1, SEEK_SET);
    return ret;
}

static int mp4_read_iods(AVFormatContext *s, const uint8_t *buf, unsigned size,
                         Mp4Descr *descr, int *descr_count, int max_descr_count)
{
    MP4DescrParseContext d;
    int ret;

    d.predefined_SLConfigDescriptor_seen = 0;

    ret = init_MP4DescrParseContext(&d, s, buf, size, descr, max_descr_count);
    if (ret < 0)
        return ret;

    ret = parse_mp4_descr(&d, avio_tell(&d.pb.pub), size, MP4IODescrTag);

    *descr_count = d.descr_count;
    return ret;
}

static int mp4_read_od(AVFormatContext *s, const uint8_t *buf, unsigned size,
                       Mp4Descr *descr, int *descr_count, int max_descr_count)
{
    MP4DescrParseContext d;
    int ret;

    ret = init_MP4DescrParseContext(&d, s, buf, size, descr, max_descr_count);
    if (ret < 0)
        return ret;

    ret = parse_mp4_descr_arr(&d, avio_tell(&d.pb.pub), size);

    *descr_count = d.descr_count;
    return ret;
}

static void m4sl_cb(MpegTSFilter *filter, const uint8_t *section,
                    int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    MpegTSSectionFilter *tssf = &filter->u.section_filter;
    SectionHeader h;
    const uint8_t *p, *p_end;
    int mp4_descr_count = 0;
    Mp4Descr mp4_descr[MAX_MP4_DESCR_COUNT] = { { 0 } };
    int i, pid;
    AVFormatContext *s = ts->stream;

    p_end = section + section_len - 4;
    p = section;
    if (parse_section_header(&h, &p, p_end) < 0)
        return;
    if (h.tid != M4OD_TID)
        return;
    if (skip_identical(&h, tssf))
        return;

    mp4_read_od(s, p, (unsigned) (p_end - p), mp4_descr, &mp4_descr_count,
                MAX_MP4_DESCR_COUNT);

    for (pid = 0; pid < NB_PID_MAX; pid++) {
        if (!ts->pids[pid])
            continue;
        for (i = 0; i < mp4_descr_count; i++) {
            PESContext *pes;
            AVStream *st;
            FFStream *sti;
            FFIOContext pb;
            if (ts->pids[pid]->es_id != mp4_descr[i].es_id)
                continue;
            if (ts->pids[pid]->type != MPEGTS_PES) {
                av_log(s, AV_LOG_ERROR, "pid %x is not PES\n", pid);
                continue;
            }
            pes = ts->pids[pid]->u.pes_filter.opaque;
            st  = pes->st;
            if (!st)
                continue;
            sti = ffstream(st);

            pes->sl = mp4_descr[i].sl;

            ffio_init_read_context(&pb, mp4_descr[i].dec_config_descr,
                                   mp4_descr[i].dec_config_descr_len);
            ff_mp4_read_dec_config_descr(s, st, &pb.pub);
            if (st->codecpar->codec_id == AV_CODEC_ID_AAC &&
                st->codecpar->extradata_size > 0)
                sti->need_parsing = 0;
            if (st->codecpar->codec_id == AV_CODEC_ID_H264 &&
                st->codecpar->extradata_size > 0)
                sti->need_parsing = 0;

            st->codecpar->codec_type = avcodec_get_type(st->codecpar->codec_id);
            sti->need_context_update = 1;
        }
    }
    for (i = 0; i < mp4_descr_count; i++)
        av_free(mp4_descr[i].dec_config_descr);
}

static void scte_data_cb(MpegTSFilter *filter, const uint8_t *section,
                    int section_len)
{
    AVProgram *prg = NULL;
    MpegTSContext *ts = filter->u.section_filter.opaque;

    int idx = ff_find_stream_index(ts->stream, filter->pid);
    if (idx < 0)
        return;

    /**
     * In case we receive an SCTE-35 packet before mpegts context is fully
     * initialized.
     */
    if (!ts->pkt)
        return;

    new_data_packet(section, section_len, ts->pkt);
    ts->pkt->stream_index = idx;
    prg = av_find_program_from_stream(ts->stream, NULL, idx);
    if (prg && prg->pcr_pid != -1 && prg->discard != AVDISCARD_ALL) {
        MpegTSFilter *f = ts->pids[prg->pcr_pid];
        if (f && f->last_pcr != -1)
            ts->pkt->pts = ts->pkt->dts = f->last_pcr/SYSTEM_CLOCK_FREQUENCY_DIVISOR;
    }
    ts->stop_parse = 1;

}

static const uint8_t opus_coupled_stream_cnt[9] = {
    1, 0, 1, 1, 2, 2, 2, 3, 3
};

static const uint8_t opus_stream_cnt[9] = {
    1, 1, 1, 2, 2, 3, 4, 4, 5,
};

static const uint8_t opus_channel_map[8][8] = {
    { 0 },
    { 0,1 },
    { 0,2,1 },
    { 0,1,2,3 },
    { 0,4,1,2,3 },
    { 0,4,1,2,3,5 },
    { 0,4,1,2,3,5,6 },
    { 0,6,1,2,3,4,5,7 },
};

int ff_parse_mpeg2_descriptor(AVFormatContext *fc, AVStream *st, int stream_type,
                              const uint8_t **pp, const uint8_t *desc_list_end,
                              Mp4Descr *mp4_descr, int mp4_descr_count, int pid,
                              MpegTSContext *ts)
{
    FFStream *const sti = ffstream(st);
    const uint8_t *desc_end;
    int desc_len, desc_tag, desc_es_id, ext_desc_tag, channels, channel_config_code;
    char language[252];
    int i;

    desc_tag = get8(pp, desc_list_end);
    if (desc_tag < 0)
        return AVERROR_INVALIDDATA;
    desc_len = get8(pp, desc_list_end);
    if (desc_len < 0)
        return AVERROR_INVALIDDATA;
    desc_end = *pp + desc_len;
    if (desc_end > desc_list_end)
        return AVERROR_INVALIDDATA;

    av_log(fc, AV_LOG_TRACE, "tag: 0x%02x len=%d\n", desc_tag, desc_len);

    if ((st->codecpar->codec_id == AV_CODEC_ID_NONE || sti->request_probe > 0) &&
        stream_type == STREAM_TYPE_PRIVATE_DATA)
        mpegts_find_stream_type(st, desc_tag, DESC_types);

    switch (desc_tag) {
    case VIDEO_STREAM_DESCRIPTOR:
        if (get8(pp, desc_end) & 0x1) {
            st->disposition |= AV_DISPOSITION_STILL_IMAGE;
        }
        break;
    case SL_DESCRIPTOR:
        desc_es_id = get16(pp, desc_end);
        if (desc_es_id < 0)
            break;
        if (ts && ts->pids[pid])
            ts->pids[pid]->es_id = desc_es_id;
        for (i = 0; i < mp4_descr_count; i++)
            if (mp4_descr[i].dec_config_descr_len &&
                mp4_descr[i].es_id == desc_es_id) {
                FFIOContext pb;
                ffio_init_read_context(&pb, mp4_descr[i].dec_config_descr,
                                       mp4_descr[i].dec_config_descr_len);
                ff_mp4_read_dec_config_descr(fc, st, &pb.pub);
                if (st->codecpar->codec_id == AV_CODEC_ID_AAC &&
                    st->codecpar->extradata_size > 0) {
                    sti->need_parsing        = 0;
                    sti->need_context_update = 1;
                }
                if (st->codecpar->codec_id == AV_CODEC_ID_MPEG4SYSTEMS)
                    mpegts_open_section_filter(ts, pid, m4sl_cb, ts, 1);
            }
        break;
    case FMC_DESCRIPTOR:
        if (get16(pp, desc_end) < 0)
            break;
        if (mp4_descr_count > 0 &&
            (st->codecpar->codec_id == AV_CODEC_ID_AAC_LATM ||
             (sti->request_probe == 0 && st->codecpar->codec_id == AV_CODEC_ID_NONE) ||
             sti->request_probe > 0) &&
            mp4_descr->dec_config_descr_len && mp4_descr->es_id == pid) {
            FFIOContext pb;
            ffio_init_read_context(&pb, mp4_descr->dec_config_descr,
                                   mp4_descr->dec_config_descr_len);
            ff_mp4_read_dec_config_descr(fc, st, &pb.pub);
            if (st->codecpar->codec_id == AV_CODEC_ID_AAC &&
                st->codecpar->extradata_size > 0) {
                sti->request_probe = sti->need_parsing = 0;
                st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                sti->need_context_update = 1;
            }
        }
        break;
    case TELETEXT_DESCRIPTOR:
        {
            uint8_t *extradata = NULL;
            int language_count = desc_len / 5, ret;

            if (desc_len > 0 && desc_len % 5 != 0)
                return AVERROR_INVALIDDATA;

            if (language_count > 0) {
                /* 4 bytes per language code (3 bytes) with comma or NUL byte should fit language buffer */
                av_assert0(language_count <= sizeof(language) / 4);

                if (st->codecpar->extradata == NULL) {
                    ret = ff_alloc_extradata(st->codecpar, language_count * 2);
                    if (ret < 0)
                        return ret;
                }

                if (st->codecpar->extradata_size < language_count * 2)
                    return AVERROR_INVALIDDATA;

                extradata = st->codecpar->extradata;

                for (i = 0; i < language_count; i++) {
                    language[i * 4 + 0] = get8(pp, desc_end);
                    language[i * 4 + 1] = get8(pp, desc_end);
                    language[i * 4 + 2] = get8(pp, desc_end);
                    language[i * 4 + 3] = ',';

                    memcpy(extradata, *pp, 2);
                    extradata += 2;

                    *pp += 2;
                }

                language[i * 4 - 1] = 0;
                av_dict_set(&st->metadata, "language", language, 0);
                sti->need_context_update = 1;
            }
        }
        break;
    case SUBTITLING_DESCRIPTOR:
        {
            /* 8 bytes per DVB subtitle substream data:
             * ISO_639_language_code (3 bytes),
             * subtitling_type (1 byte),
             * composition_page_id (2 bytes),
             * ancillary_page_id (2 bytes) */
            int language_count = desc_len / 8, ret;

            if (desc_len > 0 && desc_len % 8 != 0)
                return AVERROR_INVALIDDATA;

            if (language_count > 1) {
                avpriv_request_sample(fc, "DVB subtitles with multiple languages");
            }

            if (language_count > 0) {
                uint8_t *extradata;

                /* 4 bytes per language code (3 bytes) with comma or NUL byte should fit language buffer */
                av_assert0(language_count <= sizeof(language) / 4);

                if (st->codecpar->extradata == NULL) {
                    ret = ff_alloc_extradata(st->codecpar, language_count * 5);
                    if (ret < 0)
                        return ret;
                }

                if (st->codecpar->extradata_size < language_count * 5)
                    return AVERROR_INVALIDDATA;

                extradata = st->codecpar->extradata;

                for (i = 0; i < language_count; i++) {
                    language[i * 4 + 0] = get8(pp, desc_end);
                    language[i * 4 + 1] = get8(pp, desc_end);
                    language[i * 4 + 2] = get8(pp, desc_end);
                    language[i * 4 + 3] = ',';

                    /* hearing impaired subtitles detection using subtitling_type */
                    switch (*pp[0]) {
                    case 0x20: /* DVB subtitles (for the hard of hearing) with no monitor aspect ratio criticality */
                    case 0x21: /* DVB subtitles (for the hard of hearing) for display on 4:3 aspect ratio monitor */
                    case 0x22: /* DVB subtitles (for the hard of hearing) for display on 16:9 aspect ratio monitor */
                    case 0x23: /* DVB subtitles (for the hard of hearing) for display on 2.21:1 aspect ratio monitor */
                    case 0x24: /* DVB subtitles (for the hard of hearing) for display on a high definition monitor */
                    case 0x25: /* DVB subtitles (for the hard of hearing) with plano-stereoscopic disparity for display on a high definition monitor */
                        st->disposition |= AV_DISPOSITION_HEARING_IMPAIRED;
                        break;
                    }

                    extradata[4] = get8(pp, desc_end); /* subtitling_type */
                    memcpy(extradata, *pp, 4); /* composition_page_id and ancillary_page_id */
                    extradata += 5;

                    *pp += 4;
                }

                language[i * 4 - 1] = 0;
                av_dict_set(&st->metadata, "language", language, 0);
                sti->need_context_update = 1;
            }
        }
        break;
    case ISO_639_LANGUAGE_DESCRIPTOR:
        for (i = 0; i + 4 <= desc_len; i += 4) {
            language[i + 0] = get8(pp, desc_end);
            language[i + 1] = get8(pp, desc_end);
            language[i + 2] = get8(pp, desc_end);
            language[i + 3] = ',';
            switch (get8(pp, desc_end)) {
            case 0x01:
                st->disposition |= AV_DISPOSITION_CLEAN_EFFECTS;
                break;
            case 0x02:
                st->disposition |= AV_DISPOSITION_HEARING_IMPAIRED;
                break;
            case 0x03:
                st->disposition |= AV_DISPOSITION_VISUAL_IMPAIRED;
                st->disposition |= AV_DISPOSITION_DESCRIPTIONS;
                break;
            }
        }
        if (i && language[0]) {
            language[i - 1] = 0;
            /* don't overwrite language, as it may already have been set by
             * another, more specific descriptor (e.g. supplementary audio) */
            av_dict_set(&st->metadata, "language", language, AV_DICT_DONT_OVERWRITE);
        }
        break;
    case REGISTRATION_DESCRIPTOR:
        st->codecpar->codec_tag = bytestream_get_le32(pp);
        av_log(fc, AV_LOG_TRACE, "reg_desc=%.4s\n", (char *)&st->codecpar->codec_tag);
        if (st->codecpar->codec_id == AV_CODEC_ID_NONE || sti->request_probe > 0) {
            mpegts_find_stream_type(st, st->codecpar->codec_tag, REGD_types);
            if (st->codecpar->codec_tag == MKTAG('B', 'S', 'S', 'D'))
                sti->request_probe = 50;
        }
        break;
    case STREAM_IDENTIFIER_DESCRIPTOR:
        sti->stream_identifier = 1 + get8(pp, desc_end);
        break;
    case METADATA_DESCRIPTOR:
        if (get16(pp, desc_end) == 0xFFFF)
            *pp += 4;
        if (get8(pp, desc_end) == 0xFF) {
            st->codecpar->codec_tag = bytestream_get_le32(pp);
            if (st->codecpar->codec_id == AV_CODEC_ID_NONE)
                mpegts_find_stream_type(st, st->codecpar->codec_tag, METADATA_types);
        }
        break;
    case EXTENSION_DESCRIPTOR: /* DVB extension descriptor */
        ext_desc_tag = get8(pp, desc_end);
        if (ext_desc_tag < 0)
            return AVERROR_INVALIDDATA;
        if (st->codecpar->codec_id == AV_CODEC_ID_BIN_DATA &&
            ext_desc_tag == AC4_DESCRIPTOR_TAG_EXTENSION) {
            st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
            st->codecpar->codec_id = AV_CODEC_ID_AC4;
        }
        if (st->codecpar->codec_id == AV_CODEC_ID_OPUS &&
            ext_desc_tag == 0x80) { /* User defined (provisional Opus) */
            if (!st->codecpar->extradata) {
                st->codecpar->extradata = av_mallocz(sizeof(opus_default_extradata) +
                                                     AV_INPUT_BUFFER_PADDING_SIZE);
                if (!st->codecpar->extradata)
                    return AVERROR(ENOMEM);

                st->codecpar->extradata_size = sizeof(opus_default_extradata);
                memcpy(st->codecpar->extradata, opus_default_extradata, sizeof(opus_default_extradata));

                channel_config_code = get8(pp, desc_end);
                if (channel_config_code < 0)
                    return AVERROR_INVALIDDATA;
                if (channel_config_code <= 0x8) {
                    st->codecpar->extradata[9]  = channels = channel_config_code ? channel_config_code : 2;
                    AV_WL32(&st->codecpar->extradata[12], 48000);
                    st->codecpar->extradata[18] = channel_config_code ? (channels > 2) : /* Dual Mono */ 255;
                    st->codecpar->extradata[19] = opus_stream_cnt[channel_config_code];
                    st->codecpar->extradata[20] = opus_coupled_stream_cnt[channel_config_code];
                    memcpy(&st->codecpar->extradata[21], opus_channel_map[channels - 1], channels);
                    st->codecpar->extradata_size = st->codecpar->extradata[18] ? 21 + channels : 19;
                } else {
                    avpriv_request_sample(fc, "Opus in MPEG-TS - channel_config_code > 0x8");
                }
                sti->need_parsing = AVSTREAM_PARSE_FULL;
                sti->need_context_update = 1;
            }
        }
        if (ext_desc_tag == SUPPLEMENTARY_AUDIO_DESCRIPTOR) {
            int flags;

            if (desc_len < 1)
                return AVERROR_INVALIDDATA;
            flags = get8(pp, desc_end);

            if ((flags & 0x80) == 0) /* mix_type */
                st->disposition |= AV_DISPOSITION_DEPENDENT;

            switch ((flags >> 2) & 0x1F) { /* editorial_classification */
            case 0x01:
                st->disposition |= AV_DISPOSITION_VISUAL_IMPAIRED;
                st->disposition |= AV_DISPOSITION_DESCRIPTIONS;
                break;
            case 0x02:
                st->disposition |= AV_DISPOSITION_HEARING_IMPAIRED;
                break;
            case 0x03:
                st->disposition |= AV_DISPOSITION_VISUAL_IMPAIRED;
                break;
            }

            if (flags & 0x01) { /* language_code_present */
                if (desc_len < 4)
                    return AVERROR_INVALIDDATA;
                language[0] = get8(pp, desc_end);
                language[1] = get8(pp, desc_end);
                language[2] = get8(pp, desc_end);
                language[3] = 0;

                /* This language always has to override a possible
                 * ISO 639 language descriptor language */
                if (language[0])
                    av_dict_set(&st->metadata, "language", language, 0);
            }
        }
        break;
    case AC3_DESCRIPTOR:
        {
            int component_type_flag = get8(pp, desc_end) & (1 << 7);
            if (component_type_flag) {
                int component_type = get8(pp, desc_end);
                int service_type_mask = 0x38;  // 0b00111000
                int service_type = ((component_type & service_type_mask) >> 3);
                if (service_type == 0x02 /* 0b010 */) {
                    st->disposition |= AV_DISPOSITION_DESCRIPTIONS;
                    av_log(ts ? ts->stream : fc, AV_LOG_DEBUG, "New track disposition for id %u: %u\n", st->id, st->disposition);
                }
            }
        }
        break;
    case ENHANCED_AC3_DESCRIPTOR:
        {
            int component_type_flag = get8(pp, desc_end) & (1 << 7);
            if (component_type_flag) {
                int component_type = get8(pp, desc_end);
                int service_type_mask = 0x38;  // 0b00111000
                int service_type = ((component_type & service_type_mask) >> 3);
                if (service_type == 0x02 /* 0b010 */) {
                    st->disposition |= AV_DISPOSITION_DESCRIPTIONS;
                    av_log(ts ? ts->stream : fc, AV_LOG_DEBUG, "New track disposition for id %u: %u\n", st->id, st->disposition);
                }
            }
        }
        break;
    case DATA_COMPONENT_DESCRIPTOR:
        // STD-B24, fascicle 3, chapter 4 defines private_stream_1
        // for captions
        if (stream_type == STREAM_TYPE_PRIVATE_DATA) {
            // This structure is defined in STD-B10, part 1, listing 5.4 and
            // part 2, 6.2.20).
            // Listing of data_component_ids is in STD-B10, part 2, Annex J.
            // Component tag limits are documented in TR-B14, fascicle 2,
            // Vol. 3, Section 2, 4.2.8.1
            int actual_component_tag = sti->stream_identifier - 1;
            int picked_profile = AV_PROFILE_UNKNOWN;
            int data_component_id = get16(pp, desc_end);
            if (data_component_id < 0)
                return AVERROR_INVALIDDATA;

            switch (data_component_id) {
            case 0x0008:
                // [0x30..0x37] are component tags utilized for
                // non-mobile captioning service ("profile A").
                if (actual_component_tag >= 0x30 &&
                    actual_component_tag <= 0x37) {
                    picked_profile = AV_PROFILE_ARIB_PROFILE_A;
                }
                break;
            case 0x0012:
                // component tag 0x87 signifies a mobile/partial reception
                // (1seg) captioning service ("profile C").
                if (actual_component_tag == 0x87) {
                    picked_profile = AV_PROFILE_ARIB_PROFILE_C;
                }
                break;
            default:
                break;
            }

            if (picked_profile == AV_PROFILE_UNKNOWN)
                break;

            st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
            st->codecpar->codec_id   = AV_CODEC_ID_ARIB_CAPTION;
            if (st->codecpar->profile != picked_profile) {
                st->codecpar->profile = picked_profile;
                sti->need_context_update = 1;
            }
            sti->request_probe = 0;
            sti->need_parsing = 0;
        }
        break;
    case DOVI_VIDEO_STREAM_DESCRIPTOR:
        {
            uint32_t buf;
            AVDOVIDecoderConfigurationRecord *dovi;
            size_t dovi_size;
            int dependency_pid = -1; // Unset

            if (desc_end - *pp < 4) // (8 + 8 + 7 + 6 + 1 + 1 + 1) / 8
                return AVERROR_INVALIDDATA;

            dovi = av_dovi_alloc(&dovi_size);
            if (!dovi)
                return AVERROR(ENOMEM);

            dovi->dv_version_major = get8(pp, desc_end);
            dovi->dv_version_minor = get8(pp, desc_end);
            buf = get16(pp, desc_end);
            dovi->dv_profile        = (buf >> 9) & 0x7f;    // 7 bits
            dovi->dv_level          = (buf >> 3) & 0x3f;    // 6 bits
            dovi->rpu_present_flag  = (buf >> 2) & 0x01;    // 1 bit
            dovi->el_present_flag   = (buf >> 1) & 0x01;    // 1 bit
            dovi->bl_present_flag   =  buf       & 0x01;    // 1 bit
            if (!dovi->bl_present_flag && desc_end - *pp >= 2) {
                buf = get16(pp, desc_end);
                dependency_pid = buf >> 3; // 13 bits
            }
            if (desc_end - *pp >= 1) {  // 8 bits
                buf = get8(pp, desc_end);
                dovi->dv_bl_signal_compatibility_id = (buf >> 4) & 0x0f; // 4 bits
                dovi->dv_md_compression = (buf >> 2) & 0x03; // 2 bits
            } else {
                // 0 stands for None
                // Dolby Vision V1.2.93 profiles and levels
                dovi->dv_bl_signal_compatibility_id = 0;
                dovi->dv_md_compression = AV_DOVI_COMPRESSION_NONE;
            }

            if (!av_packet_side_data_add(&st->codecpar->coded_side_data,
                                         &st->codecpar->nb_coded_side_data,
                                         AV_PKT_DATA_DOVI_CONF,
                                         (uint8_t *)dovi, dovi_size, 0)) {
                av_free(dovi);
                return AVERROR(ENOMEM);
            }

            av_log(fc, AV_LOG_TRACE, "DOVI, version: %d.%d, profile: %d, level: %d, "
                   "rpu flag: %d, el flag: %d, bl flag: %d, dependency_pid: %d, "
                   "compatibility id: %d, compression: %d\n",
                   dovi->dv_version_major, dovi->dv_version_minor,
                   dovi->dv_profile, dovi->dv_level,
                   dovi->rpu_present_flag,
                   dovi->el_present_flag,
                   dovi->bl_present_flag,
                   dependency_pid,
                   dovi->dv_bl_signal_compatibility_id,
                   dovi->dv_md_compression);
        }
        break;
    default:
        break;
    }
    *pp = desc_end;
    return 0;
}

static AVStream *find_matching_stream(MpegTSContext *ts, int pid, unsigned int programid,
                                      int stream_identifier, int pmt_stream_idx, struct Program *p)
{
    AVFormatContext *s = ts->stream;
    AVStream *found = NULL;

    if (stream_identifier) { /* match based on "stream identifier descriptor" if present */
        for (int i = 0; i < p->nb_streams; i++) {
            if (p->streams[i].stream_identifier == stream_identifier)
                if (!found || pmt_stream_idx == i) /* fallback to idx based guess if multiple streams have the same identifier */
                    found = s->streams[p->streams[i].idx];
        }
    } else if (pmt_stream_idx < p->nb_streams) { /* match based on position within the PMT */
        found = s->streams[p->streams[pmt_stream_idx].idx];
    }

    if (found) {
        av_log(ts->stream, AV_LOG_VERBOSE,
               "re-using existing %s stream %d (pid=0x%x) for new pid=0x%x\n",
               av_get_media_type_string(found->codecpar->codec_type),
               found->index, found->id, pid);
    }

    return found;
}

static int parse_stream_identifier_desc(const uint8_t *p, const uint8_t *p_end)
{
    const uint8_t **pp = &p;
    const uint8_t *desc_list_end;
    const uint8_t *desc_end;
    int desc_list_len;
    int desc_len, desc_tag;

    desc_list_len = get16(pp, p_end);
    if (desc_list_len < 0)
        return -1;
    desc_list_len &= 0xfff;
    desc_list_end  = p + desc_list_len;
    if (desc_list_end > p_end)
        return -1;

    while (1) {
        desc_tag = get8(pp, desc_list_end);
        if (desc_tag < 0)
            return -1;
        desc_len = get8(pp, desc_list_end);
        if (desc_len < 0)
            return -1;
        desc_end = *pp + desc_len;
        if (desc_end > desc_list_end)
            return -1;

        if (desc_tag == STREAM_IDENTIFIER_DESCRIPTOR) {
            return get8(pp, desc_end);
        }
        *pp = desc_end;
    }

    return -1;
}

static int is_pes_stream(int stream_type, uint32_t prog_reg_desc)
{
    switch (stream_type) {
    case STREAM_TYPE_PRIVATE_SECTION:
    case STREAM_TYPE_ISO_IEC_14496_SECTION:
        return 0;
    case STREAM_TYPE_SCTE_DATA_SCTE_35:
        /* This User Private stream_type value is used by multiple organizations
           for different things.  ANSI/SCTE 35 splice_info_section() is a
           private_section() not a PES_packet(). */
        return !(prog_reg_desc == AV_RL32("CUEI"));
    default:
        return 1;
    }
}

static void pmt_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    MpegTSSectionFilter *tssf = &filter->u.section_filter;
    struct Program old_program;
    SectionHeader h1, *h = &h1;
    PESContext *pes;
    AVStream *st;
    const uint8_t *p, *p_end, *desc_list_end;
    int program_info_length, pcr_pid, pid, stream_type;
    int desc_list_len;
    uint32_t prog_reg_desc = 0; /* registration descriptor */
    int stream_identifier = -1;
    struct Program *prg;

    int mp4_descr_count = 0;
    Mp4Descr mp4_descr[MAX_MP4_DESCR_COUNT] = { { 0 } };
    int i;

    av_log(ts->stream, AV_LOG_TRACE, "PMT: len %i\n", section_len);
    hex_dump_debug(ts->stream, section, section_len);

    p_end = section + section_len - 4;
    p = section;
    if (parse_section_header(h, &p, p_end) < 0)
        return;
    if (h->tid != PMT_TID)
        return;
    if (!h->current_next)
        return;
    if (skip_identical(h, tssf))
        return;

    av_log(ts->stream, AV_LOG_TRACE, "sid=0x%x sec_num=%d/%d version=%d tid=%d\n",
            h->id, h->sec_num, h->last_sec_num, h->version, h->tid);

    if (!ts->scan_all_pmts && ts->skip_changes)
        return;

    prg = get_program(ts, h->id);
    if (prg)
        old_program = *prg;
    else
        clear_program(&old_program);

    if (ts->skip_unknown_pmt && !prg)
        return;
    if (prg && prg->nb_pids && prg->pids[0] != ts->current_pid)
        return;
    if (!ts->skip_clear)
        clear_avprogram(ts, h->id);
    clear_program(prg);
    add_pid_to_program(prg, ts->current_pid);

    pcr_pid = get16(&p, p_end);
    if (pcr_pid < 0)
        return;
    pcr_pid &= 0x1fff;
    add_pid_to_program(prg, pcr_pid);
    update_av_program_info(ts->stream, h->id, pcr_pid, h->version);

    av_log(ts->stream, AV_LOG_TRACE, "pcr_pid=0x%x\n", pcr_pid);

    program_info_length = get16(&p, p_end);
    if (program_info_length < 0)
        return;
    program_info_length &= 0xfff;
    while (program_info_length >= 2) {
        uint8_t tag, len;
        tag = get8(&p, p_end);
        len = get8(&p, p_end);

        av_log(ts->stream, AV_LOG_TRACE, "program tag: 0x%02x len=%d\n", tag, len);

        program_info_length -= 2;
        if (len > program_info_length)
            // something else is broken, exit the program_descriptors_loop
            break;
        program_info_length -= len;
        if (tag == IOD_DESCRIPTOR) {
            get8(&p, p_end); // scope
            get8(&p, p_end); // label
            len -= 2;
            mp4_read_iods(ts->stream, p, len, mp4_descr + mp4_descr_count,
                          &mp4_descr_count, MAX_MP4_DESCR_COUNT);
        } else if (tag == REGISTRATION_DESCRIPTOR && len >= 4) {
            prog_reg_desc = bytestream_get_le32(&p);
            len -= 4;
        }
        p += len;
    }
    p += program_info_length;
    if (p >= p_end)
        goto out;

    // stop parsing after pmt, we found header
    if (!ts->pkt)
        ts->stop_parse = 2;

    if (prg)
        prg->pmt_found = 1;

    for (i = 0; i < MAX_STREAMS_PER_PROGRAM; i++) {
        st = 0;
        pes = NULL;
        stream_type = get8(&p, p_end);
        if (stream_type < 0)
            break;
        pid = get16(&p, p_end);
        if (pid < 0)
            goto out;
        pid &= 0x1fff;
        if (pid == ts->current_pid)
            goto out;

        stream_identifier = parse_stream_identifier_desc(p, p_end) + 1;

        /* now create stream */
        if (ts->pids[pid] && ts->pids[pid]->type == MPEGTS_PES) {
            pes = ts->pids[pid]->u.pes_filter.opaque;
            if (ts->merge_pmt_versions && !pes->st) {
                st = find_matching_stream(ts, pid, h->id, stream_identifier, i, &old_program);
                if (st) {
                    pes->st = st;
                    pes->stream_type = stream_type;
                    pes->merged_st = 1;
                }
            }
            if (!pes->st) {
                pes->st = avformat_new_stream(pes->stream, NULL);
                if (!pes->st)
                    goto out;
                pes->st->id = pes->pid;
            }
            st = pes->st;
        } else if (is_pes_stream(stream_type, prog_reg_desc)) {
            if (ts->pids[pid])
                mpegts_close_filter(ts, ts->pids[pid]); // wrongly added sdt filter probably
            pes = add_pes_stream(ts, pid, pcr_pid);
            if (ts->merge_pmt_versions && pes && !pes->st) {
                st = find_matching_stream(ts, pid, h->id, stream_identifier, i, &old_program);
                if (st) {
                    pes->st = st;
                    pes->stream_type = stream_type;
                    pes->merged_st = 1;
                }
            }
            if (pes && !pes->st) {
                st = avformat_new_stream(pes->stream, NULL);
                if (!st)
                    goto out;
                st->id = pes->pid;
            }
        } else {
            int idx = ff_find_stream_index(ts->stream, pid);
            if (idx >= 0) {
                st = ts->stream->streams[idx];
            }
            if (ts->merge_pmt_versions && !st) {
                st = find_matching_stream(ts, pid, h->id, stream_identifier, i, &old_program);
            }
            if (!st) {
                st = avformat_new_stream(ts->stream, NULL);
                if (!st)
                    goto out;
                st->id = pid;
                st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
                if (stream_type == STREAM_TYPE_SCTE_DATA_SCTE_35 && prog_reg_desc == AV_RL32("CUEI")) {
                    mpegts_find_stream_type(st, stream_type, SCTE_types);
                    mpegts_open_section_filter(ts, pid, scte_data_cb, ts, 1);
                }
            }
        }

        if (!st)
            goto out;

        if (pes && pes->stream_type != stream_type)
            mpegts_set_stream_info(st, pes, stream_type, prog_reg_desc);

        add_pid_to_program(prg, pid);
        if (prg) {
            prg->streams[i].idx = st->index;
            prg->streams[i].stream_identifier = stream_identifier;
            prg->nb_streams++;
        }

        av_program_add_stream_index(ts->stream, h->id, st->index);

        desc_list_len = get16(&p, p_end);
        if (desc_list_len < 0)
            goto out;
        desc_list_len &= 0xfff;
        desc_list_end  = p + desc_list_len;
        if (desc_list_end > p_end)
            goto out;
        for (;;) {
            if (ff_parse_mpeg2_descriptor(ts->stream, st, stream_type, &p,
                                          desc_list_end, mp4_descr,
                                          mp4_descr_count, pid, ts) < 0)
                break;

            if (pes && prog_reg_desc == AV_RL32("HDMV") &&
                stream_type == STREAM_TYPE_BLURAY_AUDIO_TRUEHD && pes->sub_st) {
                av_program_add_stream_index(ts->stream, h->id,
                                            pes->sub_st->index);
                pes->sub_st->codecpar->codec_tag = st->codecpar->codec_tag;
            }
        }
        p = desc_list_end;
    }

    if (!ts->pids[pcr_pid])
        mpegts_open_pcr_filter(ts, pcr_pid);

out:
    for (i = 0; i < mp4_descr_count; i++)
        av_free(mp4_descr[i].dec_config_descr);
}

static void pat_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    MpegTSSectionFilter *tssf = &filter->u.section_filter;
    SectionHeader h1, *h = &h1;
    const uint8_t *p, *p_end;
    int sid, pmt_pid;
    int nb_prg = 0;
    AVProgram *program;

    av_log(ts->stream, AV_LOG_TRACE, "PAT:\n");
    hex_dump_debug(ts->stream, section, section_len);

    p_end = section + section_len - 4;
    p     = section;
    if (parse_section_header(h, &p, p_end) < 0)
        return;
    if (h->tid != PAT_TID)
        return;
    if (!h->current_next)
        return;
    if (ts->skip_changes)
        return;

    if (skip_identical(h, tssf))
        return;
    ts->id = h->id;

    for (;;) {
        sid = get16(&p, p_end);
        if (sid < 0)
            break;
        pmt_pid = get16(&p, p_end);
        if (pmt_pid < 0)
            break;
        pmt_pid &= 0x1fff;

        if (pmt_pid == ts->current_pid)
            break;

        av_log(ts->stream, AV_LOG_TRACE, "sid=0x%x pid=0x%x\n", sid, pmt_pid);

        if (sid == 0x0000) {
            /* NIT info */
        } else {
            MpegTSFilter *fil = ts->pids[pmt_pid];
            struct Program *prg;
            program = av_new_program(ts->stream, sid);
            if (program) {
                program->program_num = sid;
                program->pmt_pid = pmt_pid;
            }
            if (fil)
                if (   fil->type != MPEGTS_SECTION
                    || fil->pid != pmt_pid
                    || fil->u.section_filter.section_cb != pmt_cb)
                    mpegts_close_filter(ts, ts->pids[pmt_pid]);

            if (!ts->pids[pmt_pid])
                mpegts_open_section_filter(ts, pmt_pid, pmt_cb, ts, 1);
            prg = add_program(ts, sid);
            if (prg) {
                unsigned prg_idx = prg - ts->prg;
                if (prg->nb_pids && prg->pids[0] != pmt_pid)
                    clear_program(prg);
                add_pid_to_program(prg, pmt_pid);
                if (prg_idx > nb_prg)
                    FFSWAP(struct Program, ts->prg[nb_prg], ts->prg[prg_idx]);
                if (prg_idx >= nb_prg)
                    nb_prg++;
            } else
                nb_prg = 0;
        }
    }
    ts->nb_prg = nb_prg;

    if (sid < 0) {
        int i,j;
        for (j=0; j<ts->stream->nb_programs; j++) {
            for (i = 0; i < ts->nb_prg; i++)
                if (ts->prg[i].id == ts->stream->programs[j]->id)
                    break;
            if (i==ts->nb_prg && !ts->skip_clear)
                clear_avprogram(ts, ts->stream->programs[j]->id);
        }
    }
}

static void eit_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    const uint8_t *p, *p_end;
    SectionHeader h1, *h = &h1;

    /*
     * Sometimes we receive EPG packets but SDT table do not have
     * eit_pres_following or eit_sched turned on, so we open EPG
     * stream directly here.
     */
    if (!ts->epg_stream) {
        ts->epg_stream = avformat_new_stream(ts->stream, NULL);
        if (!ts->epg_stream)
            return;
        ts->epg_stream->id = EIT_PID;
        ts->epg_stream->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        ts->epg_stream->codecpar->codec_id = AV_CODEC_ID_EPG;
    }

    if (ts->epg_stream->discard == AVDISCARD_ALL)
        return;

    p_end = section + section_len - 4;
    p     = section;

    if (parse_section_header(h, &p, p_end) < 0)
        return;
    if (h->tid < EIT_TID || h->tid > OEITS_END_TID)
        return;

    av_log(ts->stream, AV_LOG_TRACE, "EIT: tid received = %.02x\n", h->tid);

    /**
     * Service_id 0xFFFF is reserved, it indicates that the current EIT table
     * is scrambled.
     */
    if (h->id == 0xFFFF) {
        av_log(ts->stream, AV_LOG_TRACE, "Scrambled EIT table received.\n");
        return;
    }

    /**
     * In case we receive an EPG packet before mpegts context is fully
     * initialized.
     */
    if (!ts->pkt)
        return;

    new_data_packet(section, section_len, ts->pkt);
    ts->pkt->stream_index = ts->epg_stream->index;
    ts->stop_parse = 1;
}

static void sdt_cb(MpegTSFilter *filter, const uint8_t *section, int section_len)
{
    MpegTSContext *ts = filter->u.section_filter.opaque;
    MpegTSSectionFilter *tssf = &filter->u.section_filter;
    SectionHeader h1, *h = &h1;
    const uint8_t *p, *p_end, *desc_list_end, *desc_end;
    int onid, val, sid, desc_list_len, desc_tag, desc_len, service_type;
    char *name, *provider_name;

    av_log(ts->stream, AV_LOG_TRACE, "SDT:\n");
    hex_dump_debug(ts->stream, section, section_len);

    p_end = section + section_len - 4;
    p     = section;
    if (parse_section_header(h, &p, p_end) < 0)
        return;
    if (h->tid != SDT_TID)
        return;
    if (!h->current_next)
        return;
    if (ts->skip_changes)
        return;
    if (skip_identical(h, tssf))
        return;

    onid = get16(&p, p_end);
    if (onid < 0)
        return;
    val = get8(&p, p_end);
    if (val < 0)
        return;
    for (;;) {
        sid = get16(&p, p_end);
        if (sid < 0)
            break;
        val = get8(&p, p_end);
        if (val < 0)
            break;
        desc_list_len = get16(&p, p_end);
        if (desc_list_len < 0)
            break;
        desc_list_len &= 0xfff;
        desc_list_end  = p + desc_list_len;
        if (desc_list_end > p_end)
            break;
        for (;;) {
            desc_tag = get8(&p, desc_list_end);
            if (desc_tag < 0)
                break;
            desc_len = get8(&p, desc_list_end);
            desc_end = p + desc_len;
            if (desc_len < 0 || desc_end > desc_list_end)
                break;

            av_log(ts->stream, AV_LOG_TRACE, "tag: 0x%02x len=%d\n",
                    desc_tag, desc_len);

            switch (desc_tag) {
            case SERVICE_DESCRIPTOR:
                service_type = get8(&p, desc_end);
                if (service_type < 0)
                    break;
                provider_name = getstr8(&p, desc_end);
                if (!provider_name)
                    break;
                name = getstr8(&p, desc_end);
                if (name) {
                    AVProgram *program = av_new_program(ts->stream, sid);
                    if (program) {
                        av_dict_set(&program->metadata, "service_name", name, 0);
                        av_dict_set(&program->metadata, "service_provider",
                                    provider_name, 0);
                    }
                }
                av_free(name);
                av_free(provider_name);
                break;
            default:
                break;
            }
            p = desc_end;
        }
        p = desc_list_end;
    }
}

static int parse_pcr(int64_t *ppcr_high, int *ppcr_low,
                     const uint8_t *packet);

/* handle one TS packet */
static int handle_packet(MpegTSContext *ts, const uint8_t *packet, int64_t pos)
{
    MpegTSFilter *tss;
    int len, pid, cc, expected_cc, cc_ok, afc, is_start, is_discontinuity,
        has_adaptation, has_payload;
    const uint8_t *p, *p_end;

    pid = AV_RB16(packet + 1) & 0x1fff;
    is_start = packet[1] & 0x40;
    tss = ts->pids[pid];
    if (ts->auto_guess && !tss && is_start) {
        add_pes_stream(ts, pid, -1);
        tss = ts->pids[pid];
    }
    if (!tss)
        return 0;
    if (is_start)
        tss->discard = discard_pid(ts, pid);
    if (tss->discard)
        return 0;
    ts->current_pid = pid;

    afc = (packet[3] >> 4) & 3;
    if (afc == 0) /* reserved value */
        return 0;
    has_adaptation   = afc & 2;
    has_payload      = afc & 1;
    is_discontinuity = has_adaptation &&
                       packet[4] != 0 && /* with length > 0 */
                       (packet[5] & 0x80); /* and discontinuity indicated */

    /* continuity check (currently not used) */
    cc = (packet[3] & 0xf);
    expected_cc = has_payload ? (tss->last_cc + 1) & 0x0f : tss->last_cc;
    cc_ok = pid == NULL_PID ||
            is_discontinuity ||
            tss->last_cc < 0 ||
            expected_cc == cc;

    tss->last_cc = cc;
    if (!cc_ok) {
        av_log(ts->stream, AV_LOG_DEBUG,
               "Continuity check failed for pid %d expected %d got %d\n",
               pid, expected_cc, cc);
        if (tss->type == MPEGTS_PES) {
            PESContext *pc = tss->u.pes_filter.opaque;
            pc->flags |= AV_PKT_FLAG_CORRUPT;
        }
    }

    if (packet[1] & 0x80) {
        av_log(ts->stream, AV_LOG_DEBUG, "Packet had TEI flag set; marking as corrupt\n");
        if (tss->type == MPEGTS_PES) {
            PESContext *pc = tss->u.pes_filter.opaque;
            pc->flags |= AV_PKT_FLAG_CORRUPT;
        }
    }

    p = packet + 4;
    if (has_adaptation) {
        int64_t pcr_h;
        int pcr_l;
        if (parse_pcr(&pcr_h, &pcr_l, packet) == 0)
            tss->last_pcr = pcr_h * SYSTEM_CLOCK_FREQUENCY_DIVISOR + pcr_l;
        /* skip adaptation field */
        p += p[0] + 1;
    }
    /* if past the end of packet, ignore */
    p_end = packet + TS_PACKET_SIZE;
    if (p >= p_end || !has_payload)
        return 0;

    if (pos >= 0) {
        av_assert0(pos >= TS_PACKET_SIZE);
        ts->pos47_full = pos - TS_PACKET_SIZE;
    }

    if (tss->type == MPEGTS_SECTION) {
        if (is_start) {
            /* pointer field present */
            len = *p++;
            if (len > p_end - p)
                return 0;
            if (len && cc_ok) {
                /* write remaining section bytes */
                write_section_data(ts, tss,
                                   p, len, 0);
                /* check whether filter has been closed */
                if (!ts->pids[pid])
                    return 0;
            }
            p += len;
            if (p < p_end) {
                write_section_data(ts, tss,
                                   p, p_end - p, 1);
            }
        } else {
            if (cc_ok) {
                write_section_data(ts, tss,
                                   p, p_end - p, 0);
            }
        }

        // stop find_stream_info from waiting for more streams
        // when all programs have received a PMT
        if (ts->stream->ctx_flags & AVFMTCTX_NOHEADER && ts->scan_all_pmts <= 0) {
            int i;
            for (i = 0; i < ts->nb_prg; i++) {
                if (!ts->prg[i].pmt_found)
                    break;
            }
            if (i == ts->nb_prg && ts->nb_prg > 0) {
                av_log(ts->stream, AV_LOG_DEBUG, "All programs have pmt, headers found\n");
                ts->stream->ctx_flags &= ~AVFMTCTX_NOHEADER;
            }
        }

    } else {
        int ret;
        // Note: The position here points actually behind the current packet.
        if (tss->type == MPEGTS_PES) {
            if ((ret = tss->u.pes_filter.pes_cb(tss, p, p_end - p, is_start,
                                                pos - ts->raw_packet_size)) < 0)
                return ret;
        }
    }

    return 0;
}

static int mpegts_resync(AVFormatContext *s, int seekback, const uint8_t *current_packet)
{
    MpegTSContext *ts = s->priv_data;
    AVIOContext *pb = s->pb;
    int c, i;
    uint64_t pos = avio_tell(pb);
    int64_t back = FFMIN(seekback, pos);

    //Special case for files like 01c56b0dc1.ts
    if (current_packet[0] == 0x80 && current_packet[12] == SYNC_BYTE && pos >= TS_PACKET_SIZE) {
        avio_seek(pb, 12 - TS_PACKET_SIZE, SEEK_CUR);
        return 0;
    }

    avio_seek(pb, -back, SEEK_CUR);

    for (i = 0; i < ts->resync_size; i++) {
        c = avio_r8(pb);
        if (avio_feof(pb))
            return AVERROR_EOF;
        if (c == SYNC_BYTE) {
            int new_packet_size, ret;
            avio_seek(pb, -1, SEEK_CUR);
            pos = avio_tell(pb);
            ret = ffio_ensure_seekback(pb, PROBE_PACKET_MAX_BUF);
            if (ret < 0)
                return ret;
            new_packet_size = get_packet_size(s);
            if (new_packet_size > 0 && new_packet_size != ts->raw_packet_size) {
                av_log(ts->stream, AV_LOG_WARNING, "changing packet size to %d\n", new_packet_size);
                ts->raw_packet_size = new_packet_size;
            }
            avio_seek(pb, pos, SEEK_SET);
            return 0;
        }
    }
    av_log(s, AV_LOG_ERROR,
           "max resync size reached, could not find sync byte\n");
    /* no sync found */
    return AVERROR_INVALIDDATA;
}

/* return AVERROR_something if error or EOF. Return 0 if OK. */
static int read_packet(AVFormatContext *s, uint8_t *buf, int raw_packet_size,
                       const uint8_t **data)
{
    AVIOContext *pb = s->pb;
    int len;

    // 192 bytes source packet that start with a 4 bytes TP_extra_header
    // followed by 188 bytes of TS packet. The sync byte is at offset 4, so skip
    // the first 4 bytes otherwise we'll end up syncing to the wrong packet.
    if (raw_packet_size == TS_DVHS_PACKET_SIZE)
        avio_skip(pb, 4);

    for (;;) {
        len = ffio_read_indirect(pb, buf, TS_PACKET_SIZE, data);
        if (len != TS_PACKET_SIZE)
            return len < 0 ? len : AVERROR_EOF;
        /* check packet sync byte */
        if ((*data)[0] != SYNC_BYTE) {
            /* find a new packet start */

            if (mpegts_resync(s, raw_packet_size, *data) < 0)
                return AVERROR(EAGAIN);
            else
                continue;
        } else {
            break;
        }
    }
    return 0;
}

static void finished_reading_packet(AVFormatContext *s, int raw_packet_size)
{
    AVIOContext *pb = s->pb;
    int skip;
    if (raw_packet_size == TS_DVHS_PACKET_SIZE)
        skip = raw_packet_size - TS_DVHS_PACKET_SIZE;
    else
        skip = raw_packet_size - TS_PACKET_SIZE;
    if (skip > 0)
        avio_skip(pb, skip);
}

static int handle_packets(MpegTSContext *ts, int64_t nb_packets)
{
    AVFormatContext *s = ts->stream;
    uint8_t packet[TS_PACKET_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    const uint8_t *data;
    int64_t packet_num;
    int ret = 0;

    if (avio_tell(s->pb) != ts->last_pos) {
        int i;
        av_log(ts->stream, AV_LOG_TRACE, "Skipping after seek\n");
        /* seek detected, flush pes buffer */
        for (i = 0; i < NB_PID_MAX; i++) {
            if (ts->pids[i]) {
                if (ts->pids[i]->type == MPEGTS_PES) {
                    PESContext *pes = ts->pids[i]->u.pes_filter.opaque;
                    av_buffer_unref(&pes->buffer);
                    pes->data_index = 0;
                    pes->state = MPEGTS_SKIP; /* skip until pes header */
                } else if (ts->pids[i]->type == MPEGTS_SECTION) {
                    ts->pids[i]->u.section_filter.last_ver = -1;
                }
                ts->pids[i]->last_cc = -1;
                ts->pids[i]->last_pcr = -1;
            }
        }
    }

    ts->stop_parse = 0;
    packet_num = 0;
    memset(packet + TS_PACKET_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    for (;;) {
        packet_num++;
        if (nb_packets != 0 && packet_num >= nb_packets ||
            ts->stop_parse > 1) {
            ret = AVERROR(EAGAIN);
            break;
        }
        if (ts->stop_parse > 0)
            break;

        ret = read_packet(s, packet, ts->raw_packet_size, &data);
        if (ret != 0)
            break;
        ret = handle_packet(ts, data, avio_tell(s->pb));
        finished_reading_packet(s, ts->raw_packet_size);
        if (ret != 0)
            break;
    }
    ts->last_pos = avio_tell(s->pb);
    return ret;
}

static int mpegts_probe(const AVProbeData *p)
{
    const int size = p->buf_size;
    int maxscore = 0;
    int sumscore = 0;
    int i;
    int check_count = size / TS_FEC_PACKET_SIZE;
#define CHECK_COUNT 10
#define CHECK_BLOCK 100

    if (!check_count)
        return 0;

    for (i = 0; i<check_count; i+=CHECK_BLOCK) {
        int left = FFMIN(check_count - i, CHECK_BLOCK);
        int score      = analyze(p->buf + TS_PACKET_SIZE     *i, TS_PACKET_SIZE     *left, TS_PACKET_SIZE     , 1);
        int dvhs_score = analyze(p->buf + TS_DVHS_PACKET_SIZE*i, TS_DVHS_PACKET_SIZE*left, TS_DVHS_PACKET_SIZE, 1);
        int fec_score  = analyze(p->buf + TS_FEC_PACKET_SIZE *i, TS_FEC_PACKET_SIZE *left, TS_FEC_PACKET_SIZE , 1);
        score = FFMAX3(score, dvhs_score, fec_score);
        sumscore += score;
        maxscore = FFMAX(maxscore, score);
    }

    sumscore = sumscore * CHECK_COUNT / check_count;
    maxscore = maxscore * CHECK_COUNT / CHECK_BLOCK;

    ff_dlog(0, "TS score: %d %d\n", sumscore, maxscore);

    if        (check_count > CHECK_COUNT && sumscore > 6) {
        return AVPROBE_SCORE_MAX   + sumscore - CHECK_COUNT;
    } else if (check_count >= CHECK_COUNT && sumscore > 6) {
        return AVPROBE_SCORE_MAX/2 + sumscore - CHECK_COUNT;
    } else if (check_count >= CHECK_COUNT && maxscore > 6) {
        return AVPROBE_SCORE_MAX/2 + sumscore - CHECK_COUNT;
    } else if (sumscore > 6) {
        return 2;
    } else {
        return 0;
    }
}

/* return the 90kHz PCR and the extension for the 27MHz PCR. return
 * (-1) if not available */
static int parse_pcr(int64_t *ppcr_high, int *ppcr_low, const uint8_t *packet)
{
    int afc, len, flags;
    const uint8_t *p;
    unsigned int v;

    afc = (packet[3] >> 4) & 3;
    if (afc <= 1)
        return AVERROR_INVALIDDATA;
    p   = packet + 4;
    len = p[0];
    p++;
    if (len == 0)
        return AVERROR_INVALIDDATA;
    flags = *p++;
    len--;
    if (!(flags & 0x10))
        return AVERROR_INVALIDDATA;
    if (len < 6)
        return AVERROR_INVALIDDATA;
    v          = AV_RB32(p);
    *ppcr_high = ((int64_t) v << 1) | (p[4] >> 7);
    *ppcr_low  = ((p[4] & 1) << 8) | p[5];
    return 0;
}

static void seek_back(AVFormatContext *s, AVIOContext *pb, int64_t pos) {

    /* NOTE: We attempt to seek on non-seekable files as well, as the
     * probe buffer usually is big enough. Only warn if the seek failed
     * on files where the seek should work. */
    if (avio_seek(pb, pos, SEEK_SET) < 0)
        av_log(s, (pb->seekable & AVIO_SEEKABLE_NORMAL) ? AV_LOG_ERROR : AV_LOG_INFO, "Unable to seek back to the start\n");
}

static int mpegts_read_header(AVFormatContext *s)
{
    MpegTSContext *ts = s->priv_data;
    AVIOContext *pb   = s->pb;
    int64_t pos, probesize = s->probesize;
    int64_t seekback = FFMAX(s->probesize, (int64_t)ts->resync_size + PROBE_PACKET_MAX_BUF);

    if (ffio_ensure_seekback(pb, seekback) < 0)
        av_log(s, AV_LOG_WARNING, "Failed to allocate buffers for seekback\n");

    pos = avio_tell(pb);
    ts->raw_packet_size = get_packet_size(s);
    if (ts->raw_packet_size <= 0) {
        av_log(s, AV_LOG_WARNING, "Could not detect TS packet size, defaulting to non-FEC/DVHS\n");
        ts->raw_packet_size = TS_PACKET_SIZE;
    }
    ts->stream     = s;
    ts->auto_guess = 0;

    if (s->iformat == &ff_mpegts_demuxer.p) {
        /* normal demux */

        /* first do a scan to get all the services */
        seek_back(s, pb, pos);

        mpegts_open_section_filter(ts, SDT_PID, sdt_cb, ts, 1);
        mpegts_open_section_filter(ts, PAT_PID, pat_cb, ts, 1);
        mpegts_open_section_filter(ts, EIT_PID, eit_cb, ts, 1);

        handle_packets(ts, probesize / ts->raw_packet_size);
        /* if could not find service, enable auto_guess */

        ts->auto_guess = 1;

        av_log(ts->stream, AV_LOG_TRACE, "tuning done\n");

        s->ctx_flags |= AVFMTCTX_NOHEADER;
    } else {
        AVStream *st;
        int pcr_pid, pid, nb_packets, nb_pcrs, ret, pcr_l;
        int64_t pcrs[2], pcr_h;
        uint8_t packet[TS_PACKET_SIZE];
        const uint8_t *data;

        /* only read packets */

        st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);
        avpriv_set_pts_info(st, 60, 1, 27000000);
        st->codecpar->codec_type = AVMEDIA_TYPE_DATA;
        st->codecpar->codec_id   = AV_CODEC_ID_MPEG2TS;

        /* we iterate until we find two PCRs to estimate the bitrate */
        pcr_pid    = -1;
        nb_pcrs    = 0;
        nb_packets = 0;
        for (;;) {
            ret = read_packet(s, packet, ts->raw_packet_size, &data);
            if (ret < 0)
                return ret;
            pid = AV_RB16(data + 1) & 0x1fff;
            if ((pcr_pid == -1 || pcr_pid == pid) &&
                parse_pcr(&pcr_h, &pcr_l, data) == 0) {
                finished_reading_packet(s, ts->raw_packet_size);
                pcr_pid = pid;
                pcrs[nb_pcrs] = pcr_h * SYSTEM_CLOCK_FREQUENCY_DIVISOR + pcr_l;
                nb_pcrs++;
                if (nb_pcrs >= 2) {
                    if (pcrs[1] - pcrs[0] > 0) {
                        /* the difference needs to be positive to make sense for bitrate computation */
                        break;
                    } else {
                        av_log(ts->stream, AV_LOG_WARNING, "invalid pcr pair %"PRId64" >= %"PRId64"\n", pcrs[0], pcrs[1]);
                        pcrs[0] = pcrs[1];
                        nb_pcrs--;
                    }
                }
            } else {
                finished_reading_packet(s, ts->raw_packet_size);
            }
            nb_packets++;
        }

        /* NOTE1: the bitrate is computed without the FEC */
        /* NOTE2: it is only the bitrate of the start of the stream */
        ts->pcr_incr = pcrs[1] - pcrs[0];
        ts->cur_pcr  = pcrs[0] - ts->pcr_incr * (nb_packets - 1);
        s->bit_rate  = TS_PACKET_SIZE * 8 * 27000000LL / ts->pcr_incr;
        st->codecpar->bit_rate = s->bit_rate;
        st->start_time      = ts->cur_pcr;
        av_log(ts->stream, AV_LOG_TRACE, "start=%0.3f pcr=%0.3f incr=%"PRId64"\n",
                st->start_time / 1000000.0, pcrs[0] / 27e6, ts->pcr_incr);
    }

    seek_back(s, pb, pos);
    return 0;
}

#define MAX_PACKET_READAHEAD ((128 * 1024) / 188)

static int mpegts_raw_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MpegTSContext *ts = s->priv_data;
    int ret, i;
    int64_t pcr_h, next_pcr_h, pos;
    int pcr_l, next_pcr_l;
    uint8_t pcr_buf[12];
    const uint8_t *data;

    if ((ret = av_new_packet(pkt, TS_PACKET_SIZE)) < 0)
        return ret;
    ret = read_packet(s, pkt->data, ts->raw_packet_size, &data);
    pkt->pos = avio_tell(s->pb);
    if (ret < 0) {
        return ret;
    }
    if (data != pkt->data)
        memcpy(pkt->data, data, TS_PACKET_SIZE);
    finished_reading_packet(s, ts->raw_packet_size);
    if (ts->mpeg2ts_compute_pcr) {
        /* compute exact PCR for each packet */
        if (parse_pcr(&pcr_h, &pcr_l, pkt->data) == 0) {
            /* we read the next PCR (XXX: optimize it by using a bigger buffer */
            pos = avio_tell(s->pb);
            for (i = 0; i < MAX_PACKET_READAHEAD; i++) {
                avio_seek(s->pb, pos + i * ts->raw_packet_size, SEEK_SET);
                avio_read(s->pb, pcr_buf, 12);
                if (parse_pcr(&next_pcr_h, &next_pcr_l, pcr_buf) == 0) {
                    /* XXX: not precise enough */
                    ts->pcr_incr =
                        ((next_pcr_h - pcr_h) * SYSTEM_CLOCK_FREQUENCY_DIVISOR + (next_pcr_l - pcr_l)) /
                        (i + 1);
                    break;
                }
            }
            avio_seek(s->pb, pos, SEEK_SET);
            /* no next PCR found: we use previous increment */
            ts->cur_pcr = pcr_h * SYSTEM_CLOCK_FREQUENCY_DIVISOR + pcr_l;
        }
        pkt->pts      = ts->cur_pcr;
        pkt->duration = ts->pcr_incr;
        ts->cur_pcr  += ts->pcr_incr;
    }
    pkt->stream_index = 0;
    return 0;
}

static int mpegts_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    MpegTSContext *ts = s->priv_data;
    int ret, i;

    pkt->size = -1;
    ts->pkt = pkt;
    ret = handle_packets(ts, 0);
    if (ret < 0) {
        av_packet_unref(ts->pkt);
        /* flush pes data left */
        for (i = 0; i < NB_PID_MAX; i++)
            if (ts->pids[i] && ts->pids[i]->type == MPEGTS_PES) {
                PESContext *pes = ts->pids[i]->u.pes_filter.opaque;
                if (pes->state == MPEGTS_PAYLOAD && pes->data_index > 0) {
                    ret = new_pes_packet(pes, pkt);
                    if (ret < 0)
                        return ret;
                    pes->state = MPEGTS_SKIP;
                    ret = 0;
                    break;
                }
            }
    }

    if (!ret && pkt->size < 0)
        ret = AVERROR_INVALIDDATA;
    return ret;
}

static void mpegts_free(MpegTSContext *ts)
{
    int i;

    clear_programs(ts);

    for (i = 0; i < FF_ARRAY_ELEMS(ts->pools); i++)
        av_buffer_pool_uninit(&ts->pools[i]);

    for (i = 0; i < NB_PID_MAX; i++)
        if (ts->pids[i])
            mpegts_close_filter(ts, ts->pids[i]);
}

static int mpegts_read_close(AVFormatContext *s)
{
    MpegTSContext *ts = s->priv_data;
    mpegts_free(ts);
    return 0;
}

static av_unused int64_t mpegts_get_pcr(AVFormatContext *s, int stream_index,
                              int64_t *ppos, int64_t pos_limit)
{
    MpegTSContext *ts = s->priv_data;
    int64_t pos, timestamp;
    uint8_t buf[TS_PACKET_SIZE];
    int pcr_l, pcr_pid =
        ((PESContext *)s->streams[stream_index]->priv_data)->pcr_pid;
    int pos47 = ts->pos47_full % ts->raw_packet_size;
    pos =
        ((*ppos + ts->raw_packet_size - 1 - pos47) / ts->raw_packet_size) *
        ts->raw_packet_size + pos47;
    while(pos < pos_limit) {
        if (avio_seek(s->pb, pos, SEEK_SET) < 0)
            return AV_NOPTS_VALUE;
        if (avio_read(s->pb, buf, TS_PACKET_SIZE) != TS_PACKET_SIZE)
            return AV_NOPTS_VALUE;
        if (buf[0] != SYNC_BYTE) {
            if (mpegts_resync(s, TS_PACKET_SIZE, buf) < 0)
                return AV_NOPTS_VALUE;
            pos = avio_tell(s->pb);
            continue;
        }
        if ((pcr_pid < 0 || (AV_RB16(buf + 1) & 0x1fff) == pcr_pid) &&
            parse_pcr(&timestamp, &pcr_l, buf) == 0) {
            *ppos = pos;
            return timestamp;
        }
        pos += ts->raw_packet_size;
    }

    return AV_NOPTS_VALUE;
}

static int64_t mpegts_get_dts(AVFormatContext *s, int stream_index,
                              int64_t *ppos, int64_t pos_limit)
{
    MpegTSContext *ts = s->priv_data;
    AVPacket *pkt;
    int64_t pos;
    int pos47 = ts->pos47_full % ts->raw_packet_size;
    pos = ((*ppos  + ts->raw_packet_size - 1 - pos47) / ts->raw_packet_size) * ts->raw_packet_size + pos47;
    ff_read_frame_flush(s);
    if (avio_seek(s->pb, pos, SEEK_SET) < 0)
        return AV_NOPTS_VALUE;
    pkt = av_packet_alloc();
    if (!pkt)
        return AV_NOPTS_VALUE;
    while(pos < pos_limit) {
        int ret = av_read_frame(s, pkt);
        if (ret < 0) {
            av_packet_free(&pkt);
            return AV_NOPTS_VALUE;
        }
        if (pkt->dts != AV_NOPTS_VALUE && pkt->pos >= 0) {
            ff_reduce_index(s, pkt->stream_index);
            av_add_index_entry(s->streams[pkt->stream_index], pkt->pos, pkt->dts, 0, 0, AVINDEX_KEYFRAME /* FIXME keyframe? */);
            if (pkt->stream_index == stream_index && pkt->pos >= *ppos) {
                int64_t dts = pkt->dts;
                *ppos = pkt->pos;
                av_packet_free(&pkt);
                return dts;
            }
        }
        pos = pkt->pos;
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return AV_NOPTS_VALUE;
}

/**************************************************************/
/* parsing functions - called from other demuxers such as RTP */

MpegTSContext *avpriv_mpegts_parse_open(AVFormatContext *s)
{
    MpegTSContext *ts;

    ts = av_mallocz(sizeof(MpegTSContext));
    if (!ts)
        return NULL;
    /* no stream case, currently used by RTP */
    ts->raw_packet_size = TS_PACKET_SIZE;
    ts->max_packet_size = 2048000;
    ts->stream = s;
    ts->auto_guess = 1;

    mpegts_open_section_filter(ts, SDT_PID, sdt_cb, ts, 1);
    mpegts_open_section_filter(ts, PAT_PID, pat_cb, ts, 1);
    mpegts_open_section_filter(ts, EIT_PID, eit_cb, ts, 1);

    return ts;
}

/* return the consumed length if a packet was output, or -1 if no
 * packet is output */
int avpriv_mpegts_parse_packet(MpegTSContext *ts, AVPacket *pkt,
                               const uint8_t *buf, int len)
{
    int len1;

    len1 = len;
    ts->pkt = pkt;
    for (;;) {
        ts->stop_parse = 0;
        if (len < TS_PACKET_SIZE)
            return AVERROR_INVALIDDATA;
        if (buf[0] != SYNC_BYTE) {
            buf++;
            len--;
        } else {
            handle_packet(ts, buf, len1 - len + TS_PACKET_SIZE);
            buf += TS_PACKET_SIZE;
            len -= TS_PACKET_SIZE;
            if (ts->stop_parse == 1)
                break;
        }
    }
    return len1 - len;
}

void avpriv_mpegts_parse_close(MpegTSContext *ts)
{
    mpegts_free(ts);
    av_free(ts);
}

const FFInputFormat ff_mpegts_demuxer = {
    .p.name         = "mpegts",
    .p.long_name    = NULL_IF_CONFIG_SMALL("MPEG-TS (MPEG-2 Transport Stream)"),
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT,
    .p.priv_class   = &mpegts_class,
    .priv_data_size = sizeof(MpegTSContext),
    .read_probe     = mpegts_probe,
    .read_header    = mpegts_read_header,
    .read_packet    = mpegts_read_packet,
    .read_close     = mpegts_read_close,
    .read_timestamp = mpegts_get_dts,
    .flags_internal  = FF_INFMT_FLAG_PREFER_CODEC_FRAMERATE,
};

const FFInputFormat ff_mpegtsraw_demuxer = {
    .p.name         = "mpegtsraw",
    .p.long_name    = NULL_IF_CONFIG_SMALL("raw MPEG-TS (MPEG-2 Transport Stream)"),
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT,
    .p.priv_class   = &mpegtsraw_class,
    .priv_data_size = sizeof(MpegTSContext),
    .read_header    = mpegts_read_header,
    .read_packet    = mpegts_raw_read_packet,
    .read_close     = mpegts_read_close,
    .read_timestamp = mpegts_get_dts,
    .flags_internal  = FF_INFMT_FLAG_PREFER_CODEC_FRAMERATE,
};
