#include <string.h>

#include <SDL_thread.h>

#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
#include <libavutil/frame.h>
#include <libavutil/common.h>

#include "demuxer.h"

static const char *input_name_for_log(const Demuxer *d)
{
    return (d && d->input_url) ? d->input_url : "";
}

/* AVFormatContext::interrupt_callback: must mirror stream_close()'s ordering:
 * set demuxer.abort_request before demuxer_stop so blocking I/O exits quickly. */
static int decode_interrupt_cb(void *ctx)
{
    Demuxer *demuxer = (Demuxer *)ctx;
    return demuxer && demuxer->abort_request;
}

static void print_error(const char *d, int err)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, errbuf, sizeof(errbuf));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", d, errbuf);
}

int demuxer_init(Demuxer *d, const char *input_url)
{
    memset(d, 0, sizeof(*d));

    if (!input_url)
        return AVERROR(EINVAL);

    d->seek_mode          = -1;
    d->eof                = 0;
    d->max_frame_duration = 0.0;

    memset(d->st_index, -1, sizeof(d->st_index));

    d->input_url = av_strdup(input_url);
    if (!d->input_url)
        return AVERROR(ENOMEM);

    d->wait_mutex = SDL_CreateMutex();
    if (!d->wait_mutex) {
        av_freep(&d->input_url);
        memset(d, 0, sizeof(*d));
        return AVERROR(ENOMEM);
    }

    d->continue_read_thread = SDL_CreateCond();
    if (!d->continue_read_thread) {
        SDL_DestroyMutex(d->wait_mutex);
        d->wait_mutex = NULL;
        av_freep(&d->input_url);
        memset(d, 0, sizeof(*d));
        return AVERROR(ENOMEM);
    }

    d->ic = avformat_alloc_context();
    if (!d->ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        SDL_DestroyMutex(d->wait_mutex);
        d->wait_mutex = NULL;
        SDL_DestroyCond(d->continue_read_thread);
        d->continue_read_thread = NULL;
        av_freep(&d->input_url);
        memset(d, 0, sizeof(*d));
        return AVERROR(ENOMEM);
    }
    d->ic->interrupt_callback.callback = decode_interrupt_cb;
    d->ic->interrupt_callback.opaque  = d;

    return 0;
}

void demuxer_destroy(Demuxer *d)
{
    if (!d)
        return;

    avformat_close_input(&d->ic);
    av_freep(&d->input_url);
    if (d->wait_mutex) {
        SDL_DestroyMutex(d->wait_mutex);
        d->wait_mutex = NULL;
    }
    if (d->continue_read_thread) {
        SDL_DestroyCond(d->continue_read_thread);
        d->continue_read_thread = NULL;
    }
    memset(d, 0, sizeof(*d));
}

void demuxer_notify_continue_read(Demuxer *d)
{
    if (d && d->continue_read_thread) {
        SDL_CondSignal(d->continue_read_thread);
    }
}

int demuxer_stream_index(const Demuxer *d, enum AVMediaType type)
{
    if (!d || (unsigned int)type >= AVMEDIA_TYPE_NB)
        return -1;
    return d->st_index[type];
}

int demuxer_open_input(Demuxer *d, AVDictionary **format_opts)
{
    if (!d || !d->ic)
        return AVERROR(EINVAL);

    AVDictionary *transient = NULL;
    AVDictionary **opt_ref  = format_opts ? format_opts : &transient;

    if (!av_dict_get(*opt_ref, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE))
        av_dict_set(opt_ref, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);

    int err = avformat_open_input(&d->ic, input_name_for_log(d), NULL, opt_ref);
    if (err < 0)
        print_error(input_name_for_log(d), err);

    if (!format_opts)
        av_dict_free(&transient);

    return err;
}

int demuxer_find_stream_info(Demuxer *d, AVDictionary **stream_opts)
{
    if (!d || !d->ic)
        return AVERROR(EINVAL);

    int err = avformat_find_stream_info(d->ic, stream_opts);
    if (err < 0)
        av_log(NULL, AV_LOG_WARNING,
               "%s: could not find codec parameters (%d)\n",
               input_name_for_log(d), err);
    return err;
}

void demuxer_set_stream_metadata_fn(Demuxer *d, DemuxerStreamMetadataFn fn, void *opaque)
{
    if (!d)
        return;
    d->on_stream_metadata         = fn;
    d->stream_metadata_opaque     = opaque;
}

void demuxer_io_reset_eof(Demuxer *d)
{
    if (!d || !d->ic || !d->ic->pb)
        return;
    d->ic->pb->eof_reached = 0;
}

int demuxer_should_use_byte_seek(const Demuxer *d)
{
    if (!d || !d->ic || !d->ic->iformat)
        return 0;

    AVFormatContext *ic = d->ic;

    if (ic->iformat->flags & AVFMT_NO_BYTE_SEEK)
        return 0;

    int has_discontinuity = !!(ic->iformat->flags & AVFMT_TS_DISCONT);
    int is_ogg = (strcmp("ogg", ic->iformat->name) == 0);

    return has_discontinuity && !is_ogg;
}

double demuxer_get_max_gap(const Demuxer *d)
{
    if (!d || !d->ic || !d->ic->iformat)
        return 3600.0;

    AVFormatContext *ic = d->ic;
    if (ic->iformat->flags & AVFMT_TS_DISCONT)
        return 10.0;
    return 3600.0;
}

int demuxer_is_realtime(const Demuxer *d)
{
    if (!d)
        return -1;

    AVFormatContext *ic = d->ic;
    if (!ic)
        return -1;

    if (ic->iformat && ic->iformat->name &&
        (!strcmp(ic->iformat->name, "rtp")
         || !strcmp(ic->iformat->name, "rtsp")
         || !strcmp(ic->iformat->name, "sdp"))) {
        return 1;
    }

    if (ic->pb && ic->url && (!strncmp(ic->url, "rtp:", 4)
                             || !strncmp(ic->url, "udp:", 4))) {
        return 1;
    }
    return 0;
}

int demuxer_find_stream_components(Demuxer *d)
{
    if (!d || !d->ic) {
        av_log(NULL, AV_LOG_ERROR, "AVFormatContext not initialized\n");
        return AVERROR(EINVAL);
    }

    for (int i = 0; i < d->ic->nb_streams; i++) {
        AVStream *st = d->ic->streams[i];
        st->discard = AVDISCARD_ALL;
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }

    d->st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(d->ic, AVMEDIA_TYPE_VIDEO,
                            d->st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    d->st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(d->ic, AVMEDIA_TYPE_AUDIO,
                            d->st_index[AVMEDIA_TYPE_AUDIO],
                            d->st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);
    d->st_index[AVMEDIA_TYPE_SUBTITLE] =
        av_find_best_stream(d->ic, AVMEDIA_TYPE_SUBTITLE,
                            d->st_index[AVMEDIA_TYPE_SUBTITLE],
                            (d->st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                             d->st_index[AVMEDIA_TYPE_AUDIO] :
                             d->st_index[AVMEDIA_TYPE_VIDEO]),
                            NULL, 0);
    return 0;
}

int demuxer_is_realtime_network_protocol(const Demuxer *d)
{
    if (!d || !d->ic)
        return 0;

    AVFormatContext *ic = d->ic;
    const char      *name = input_name_for_log(d);

    if (ic->iformat && ic->iformat->name && !strcmp(ic->iformat->name, "rtsp"))
        return 1;

    if (ic->pb && name && !strncmp(name, "mmsh:", 5))
        return 1;
    return 0;
}

int demuxer_read_packet(Demuxer *d, AVPacket *pkt)
{
    if (!d || !d->ic || !pkt)
        return AVERROR(EINVAL);
    return av_read_frame(d->ic, pkt);
}

int demuxer_is_io_error(const Demuxer *d)
{
    if (!d)
        return 0;
    return d->ic && d->ic->pb && d->ic->pb->error;
}

int demuxer_should_handle_eof(const Demuxer *d, int ret)
{
    if (!d || !d->ic)
        return 0;
    if (ret == AVERROR_EOF || (d->ic->pb && avio_feof(d->ic->pb)))
        return !d->eof;
    return 0;
}

void demuxer_handle_pkt_stream_events(Demuxer *d, AVPacket *pkt)
{
    AVFormatContext *ic;
    if (!d)
        return;
    ic = d->ic;
    if (!ic || !pkt)
        return;

    if (pkt->stream_index < 0 || pkt->stream_index >= ic->nb_streams)
        return;

    AVStream *st = ic->streams[pkt->stream_index];
    if (st->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
        if (d->on_stream_metadata)
            d->on_stream_metadata(d->stream_metadata_opaque, pkt->stream_index,
                                  st->metadata);
        /* Drop the edge so incremental updates do not accumulate without fresh packets. */
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }
}

int demuxer_remote_play(Demuxer *d)
{
    if (!d || !d->ic)
        return AVERROR(EINVAL);
    return av_read_play(d->ic);
}

int demuxer_remote_pause(Demuxer *d)
{
    if (!d || !d->ic)
        return AVERROR(EINVAL);
    return av_read_pause(d->ic);
}

int demuxer_get_stream_width(const Demuxer *d, int stream_index)
{
    AVFormatContext *ic = d->ic;
    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
        return 0;
    AVStream *st = ic->streams[stream_index];
    if (!st || !st->codecpar)
        return 0;
    return st->codecpar->width;
}

int demuxer_get_stream_height(const Demuxer *d, int stream_index)
{
    AVFormatContext *ic = d->ic;
    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
        return 0;
    AVStream *st = ic->streams[stream_index];
    if (!st || !st->codecpar)
        return 0;
    return st->codecpar->height;
}

AVRational demuxer_guess_sample_aspect_ratio(const Demuxer *d, int stream_index,
                                             AVFrame *frame)
{
    AVFormatContext *ic = d->ic;
    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
        return (AVRational){0, 1};
    AVStream *st = ic->streams[stream_index];
    if (!st)
        return (AVRational){0, 1};
    return av_guess_sample_aspect_ratio(ic, st, frame);
}

AVRational demuxer_guess_frame_rate(const Demuxer *d, int stream_index, AVFrame *frame)
{
    AVFormatContext *ic = d->ic;
    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
        return (AVRational){0, 1};
    AVStream *st = ic->streams[stream_index];
    if (!st)
        return (AVRational){0, 1};
    return av_guess_frame_rate(ic, st, frame);
}

int demuxer_start(Demuxer *d, int (*read_thread_fn)(void *), void *arg)
{
    if (!d || !read_thread_fn)
        return AVERROR(EINVAL);
    d->read_tid = SDL_CreateThread(read_thread_fn, "read_thread", arg);
    if (!d->read_tid)
        return AVERROR(ENOMEM);
    return 0;
}

int demuxer_seek_file(Demuxer *d, int stream_index, int64_t min_ts,
                      int64_t ts, int64_t max_ts, int flags)
{
    if (!d || !d->ic)
        return AVERROR(EINVAL);
    return avformat_seek_file(d->ic, stream_index, min_ts, ts, max_ts, flags);
}

double demuxer_get_duration_seconds(const Demuxer *d)
{
    if (!d || !d->ic || d->ic->duration <= 0)
        return -1.0;
    return d->ic->duration / (double)AV_TIME_BASE;
}

int demuxer_stream_is_seekable(const Demuxer *d)
{
    if (!d || !d->ic)
        return 0;

    if (d->ic->duration > 0)
        return 1;

    return d->ic->pb && avio_size(d->ic->pb) > 0;
}

float demuxer_get_byte_progress(const Demuxer *d)
{
    if (!d || !d->ic || !d->ic->pb)
        return -1.0f;
    int64_t size = avio_size(d->ic->pb);
    int64_t pos  = avio_tell(d->ic->pb);
    if (size > 0 && pos >= 0)
        return av_clipf((float)pos / (float)size, 0.0f, 1.0f);
    return -1.0f;
}

void demuxer_stop(Demuxer *d)
{
    if (!d)
        return;
    if (d->read_tid) {
        SDL_WaitThread(d->read_tid, NULL);
        d->read_tid = NULL;
    }
}

void demuxer_wait_for_continue_reading(Demuxer *d, int32_t timeout_ms)
{
    if (!d || !d->wait_mutex || !d->continue_read_thread)
        return;

    SDL_LockMutex(d->wait_mutex);
    SDL_CondWaitTimeout(d->continue_read_thread, d->wait_mutex, timeout_ms);
    SDL_UnlockMutex(d->wait_mutex);
}
