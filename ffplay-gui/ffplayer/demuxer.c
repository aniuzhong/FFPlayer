#include <errno.h>
#include <string.h>

#include <SDL_thread.h>

#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
#include <libavutil/frame.h>

#include "demuxer.h"

typedef struct Demuxer {
    AVFormatContext *ic;
    int              seek_mode;
    int              abort_request;
    int              eof;
    double           max_frame_duration;
    char            *input_url;
    SDL_Thread      *read_tid;
    SDL_mutex       *wait_mutex;
    SDL_cond        *continue_read_thread;
    int              queue_attachments_req;
    int              read_pause_return;
    int              st_index[AVMEDIA_TYPE_NB];
} Demuxer;

static int decode_interrupt_cb(void *ctx)
{
    Demuxer *demuxer = (Demuxer *)ctx;
    return demuxer_is_aborted(demuxer);
}

static void print_error(const char *d, int err)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, errbuf, sizeof(errbuf));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", d, errbuf);
}

Demuxer *demuxer_create(const char *input_url)
{
    if (!input_url)
        return NULL;

    Demuxer *d = av_mallocz(sizeof(Demuxer));
    if (!d)
        return NULL;

    d->seek_mode          = -1;   // auto-detect
    d->eof                = 0;
    d->max_frame_duration = 0.0;

    memset(d->st_index, -1, sizeof(d->st_index));

    d->input_url = av_strdup(input_url);
    if (!d->input_url)
        goto fail;

    d->wait_mutex = SDL_CreateMutex();
    if (!d->wait_mutex)
        goto fail;

    d->continue_read_thread = SDL_CreateCond();
    if (!d->continue_read_thread)
        goto fail;

    d->ic = avformat_alloc_context();
    if (!d->ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        goto fail;
    }
    d->ic->interrupt_callback.callback = decode_interrupt_cb;
    d->ic->interrupt_callback.opaque = d;

    return d;

fail:
    if (d) {
        if (d->ic) {
            avformat_free_context(d->ic);
        }
        av_freep(&d->input_url);
        if (d->wait_mutex)
            SDL_DestroyMutex(d->wait_mutex);
        if (d->continue_read_thread)
            SDL_DestroyCond(d->continue_read_thread);
        av_free(d);
    }
    return NULL;
}

void demuxer_free(Demuxer **pp)
{
    if (!pp || !*pp)
        return;

    Demuxer *d = *pp;
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
    av_free(d);
    *pp = NULL;
}

const char *demuxer_get_input_name(const Demuxer *d)
{
    if (!d || !d->input_url)
        return "";
    return d->input_url;
}

AVFormatContext *demuxer_get_format_context(const Demuxer *d)
{
    /* TODO Consider a better wrapper instead of direct access to d->ic */
    if (!d)
        return NULL;
    return d->ic;
}

int demuxer_is_eof(const Demuxer *d)
{
    if (!d)
        return 0;
    return d->eof;
}

void demuxer_set_eof(Demuxer *d, int eof)
{
    if (!d)
        return;
    d->eof = eof;
}

void demuxer_notify_continue_read(Demuxer *d)
{
    if (d && d->continue_read_thread) {
        SDL_CondSignal(d->continue_read_thread);
    }
}

void demuxer_request_abort(Demuxer *d)
{
    if (!d)
        return;
    d->abort_request = 1;
}

int demuxer_is_aborted(const Demuxer *d)
{
    if (!d)
        return 0;
    return d->abort_request;
}

int demuxer_get_seek_mode(const Demuxer *d)
{
    if (!d || d->seek_mode < 0)
        return 0;
    return d->seek_mode;
}

void demuxer_set_seek_mode(Demuxer *d, int seek_mode)
{
    if (!d)
        return;
    d->seek_mode = seek_mode;
}

double demuxer_get_max_frame_duration(const Demuxer *d)
{
    if (!d)
        return 0.0;
    return d->max_frame_duration;
}

void demuxer_set_max_frame_duration(Demuxer *d, double max_frame_duration)
{
    if (!d)
        return;
    d->max_frame_duration = max_frame_duration;
}

double *demuxer_get_max_frame_duration_ptr(Demuxer *d)
{
    if (!d)
        return NULL;
    return &d->max_frame_duration;
}

SDL_cond *demuxer_get_continue_read_thread(const Demuxer *d)
{
    if (!d)
        return NULL;
    return d->continue_read_thread;
}

int demuxer_get_queue_attachments_req(const Demuxer *d)
{
    if (!d)
        return 0;
    return d->queue_attachments_req;
}

void demuxer_set_queue_attachments_req(Demuxer *d, int req)
{
    if (!d)
        return;
    d->queue_attachments_req = req;
}

int demuxer_get_read_pause_return(const Demuxer *d)
{
    if (!d)
        return 0;
    return d->read_pause_return;
}

void demuxer_set_read_pause_return(Demuxer *d, int ret)
{
    if (!d)
        return;
    d->read_pause_return = ret;
}

int demuxer_open_input(Demuxer *d, AVDictionary **options)
{
    AVDictionary *open_opts = NULL;
    if (!av_dict_get(open_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE))
        av_dict_set(&open_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    int err = avformat_open_input(&d->ic, demuxer_get_input_name(d), NULL, &open_opts);
    av_dict_free(&open_opts);
    open_opts = NULL;
    if (err < 0) {
        print_error(demuxer_get_input_name(d), err);
        return -1;
    }
    return 0;
}

int demuxer_find_stream_info(Demuxer *d, AVDictionary **options)
{
    int err = avformat_find_stream_info(d->ic, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "%s: could not find codec parameters\n", demuxer_get_input_name(d));
        return -1;
    }
    return 0;
}

void demuxer_io_reset_eof(Demuxer *d)
{
    // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    if (d->ic->pb)
        d->ic->pb->eof_reached = 0;
}

int demuxer_should_use_byte_seek(Demuxer* d)
{
    if (!d || !d->ic || !d->ic->iformat)
        return 0;

    AVFormatContext *ic = d->ic;

    // 1. Baseline: If the format explicitly flags byte seeking as unsupported
    //    return false.
    if (ic->iformat->flags & AVFMT_NO_BYTE_SEEK) {
        return 0;
    }

    // 2. Core logic: Check for timestamp discontinuities.
    //    '!!' ensures the bitwise result is normalized to a boolean 0 or 1.
    int has_discontinuity = !!(ic->iformat->flags & AVFMT_TS_DISCONT);

    // 3. Exception: Even with discontinuity flags,
    //    Ogg's internal page structure makes byte seeking perform poorly.
    int is_ogg = (strcmp("ogg", ic->iformat->name) == 0);

    // Final: Enable byte seeking only if
    //        the format supports it,
    //        exhibits timestamp discontinuities,
    //        and is not the Ogg format.
    return has_discontinuity && !is_ogg;
}

double demuxer_get_max_gap(Demuxer* d)
{
    if (!d || !d->ic || !d->ic->iformat)
        return 3600.0;

    AVFormatContext *ic = d->ic;
    if (ic->iformat->flags & AVFMT_TS_DISCONT)
        return 10.0;   // Unstable timestamps: be strict
    return 3600.0;     // Stable timestamps: be lenient
}

int demuxer_is_realtime(Demuxer *d)
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

    if (ic->pb && (!strncmp(ic->url, "rtp:", 4)
        || !strncmp(ic->url, "udp:", 4))) {
        return 1;
    }
    return 0;
}

int demuxer_find_stream_components(Demuxer *d)
{
    if (!d->ic) {
        av_log(NULL, AV_LOG_ERROR, "AVFormatContext not initialized\n");
        return -1;
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

int demuxer_get_stream_index(const Demuxer *d, enum AVMediaType type)
{
    if (!d || (unsigned int)type >= AVMEDIA_TYPE_NB) {
        return -1; 
    }
    return d->st_index[type];
}

int demuxer_is_realtime_network_protocol(Demuxer* d)
{
    if (!d || !d->ic)
        return 0;

    AVFormatContext *ic = d->ic;
    const char *name = demuxer_get_input_name(d);

    // Check for RTSP (Real Time Streaming Protocol)
    if (ic->iformat && ic->iformat->name && !strcmp(ic->iformat->name, "rtsp")) {
        return 1;
    }

    // Check for MMSH (Microsoft Media Server over HTTP)
    if (ic->pb && name && !strncmp(name, "mmsh:", 5)) {
        return 1;
    }
    return 0;
}

int demuxer_read_packet(Demuxer *d, AVPacket *pkt)
{
    if (!d || !pkt)
        return AVERROR(EINVAL);
    return av_read_frame(d->ic, pkt);
}

int demuxer_is_io_error(Demuxer *d)
{
    if (!d)
        return 0;
    return d->ic && d->ic->pb && d->ic->pb->error;
}

int demuxer_should_handle_eof(Demuxer *d, int ret)
{
    if (!d || !d->ic)
        return 0;
    if (ret == AVERROR_EOF || (d->ic->pb && avio_feof(d->ic->pb)))
        return !demuxer_is_eof(d);
    return 0;
}

void demuxer_handle_pkt_stream_events(Demuxer *d, AVPacket *pkt)
{
    AVFormatContext *ic = d->ic;
    if (!ic || !pkt)
        return;

    if (pkt->stream_index < 0 || pkt->stream_index >= ic->nb_streams)
        return;

    AVStream *st = ic->streams[pkt->stream_index];
    if (st->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
        // Handle metadata update event here if needed
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

AVRational demuxer_guess_sample_aspect_ratio(const Demuxer *d, int stream_index, AVFrame *frame)
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

int demuxer_seek_file(Demuxer *d, int stream_index, int64_t min_ts, int64_t ts, int64_t max_ts, int flags)
{
    if (!d || !d->ic)
        return AVERROR(EINVAL);
    return avformat_seek_file(d->ic, stream_index, min_ts, ts, max_ts, flags);
}

double demuxer_get_duration_seconds(Demuxer *d)
{
    if (!d || !d->ic || d->ic->duration <= 0)
        return -1.0;
    return d->ic->duration / (double)AV_TIME_BASE;
}

int demuxer_stream_is_seekable(const Demuxer *d)
{
    if (!d || !d->ic)
        return 0;

    // If the duration is known, the stream is generally seekable
    if (d->ic->duration > 0)
        return 1;

    // Fallback: check if the underlying I/O layer has a finite size
    // (Useful for local files with missing duration metadata)
    return d->ic->pb && avio_size(d->ic->pb) > 0;
}

float demuxer_get_byte_progress(const Demuxer *d)
{
    if (!d || !d->ic || !d->ic->pb)
        return -1.0f;
    int64_t size = avio_size(d->ic->pb);
    int64_t pos = avio_tell(d->ic->pb);
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
