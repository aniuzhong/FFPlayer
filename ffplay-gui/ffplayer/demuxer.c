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

static void print_error(const char *filename, int err)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, errbuf, sizeof(errbuf));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf);
}

Demuxer *demuxer_create(const char *input_url)
{
    if (!input_url)
        return NULL;

    Demuxer *demuxer = av_mallocz(sizeof(Demuxer));
    if (!demuxer)
        return NULL;

    demuxer->seek_mode          = -1;   // auto-detect
    demuxer->eof                = 0;
    demuxer->max_frame_duration = 0.0;

    memset(demuxer->st_index, -1, sizeof(demuxer->st_index));

    demuxer->input_url = av_strdup(input_url);
    if (!demuxer->input_url)
        goto fail;

    demuxer->wait_mutex = SDL_CreateMutex();
    if (!demuxer->wait_mutex)
        goto fail;

    demuxer->continue_read_thread = SDL_CreateCond();
    if (!demuxer->continue_read_thread)
        goto fail;

    demuxer->ic = avformat_alloc_context();
    if (!demuxer->ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        goto fail;
    }
    demuxer->ic->interrupt_callback.callback = decode_interrupt_cb;
    demuxer->ic->interrupt_callback.opaque = demuxer;

    return demuxer;

fail:
    if (demuxer) {
        if (demuxer->ic) {
            avformat_free_context(demuxer->ic);
        }
        av_freep(&demuxer->input_url);
        if (demuxer->wait_mutex)
            SDL_DestroyMutex(demuxer->wait_mutex);
        if (demuxer->continue_read_thread)
            SDL_DestroyCond(demuxer->continue_read_thread);
        av_free(demuxer);
    }
    return NULL;
}

void demuxer_free(Demuxer **demuxer)
{
    if (!demuxer || !*demuxer)
        return;

    Demuxer *d = *demuxer;
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
    *demuxer = NULL;
}

const char *demuxer_get_input_name(const Demuxer *demuxer)
{
    if (!demuxer || !demuxer->input_url)
        return "";
    return demuxer->input_url;
}

AVFormatContext *demuxer_get_format_context(const Demuxer *demuxer)
{
    /* TODO Consider a better wrapper instead of direct access to demuxer->ic */
    if (!demuxer)
        return NULL;
    return demuxer->ic;
}

int demuxer_is_eof(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->eof;
}

void demuxer_set_eof(Demuxer *demuxer, int eof)
{
    if (!demuxer)
        return;
    demuxer->eof = eof;
}

void demuxer_notify_continue_read(Demuxer *demuxer)
{
    if (demuxer && demuxer->continue_read_thread) {
        SDL_CondSignal(demuxer->continue_read_thread);
    }
}

void demuxer_request_abort(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    demuxer->abort_request = 1;
}

int demuxer_is_aborted(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->abort_request;
}

int demuxer_get_seek_mode(const Demuxer *demuxer)
{
    if (!demuxer || demuxer->seek_mode < 0)
        return 0;
    return demuxer->seek_mode;
}

void demuxer_set_seek_mode(Demuxer *demuxer, int seek_mode)
{
    if (!demuxer)
        return;
    demuxer->seek_mode = seek_mode;
}

double demuxer_get_max_frame_duration(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0.0;
    return demuxer->max_frame_duration;
}

void demuxer_set_max_frame_duration(Demuxer *demuxer, double max_frame_duration)
{
    if (!demuxer)
        return;
    demuxer->max_frame_duration = max_frame_duration;
}

double *demuxer_get_max_frame_duration_ptr(Demuxer *demuxer)
{
    if (!demuxer)
        return NULL;
    return &demuxer->max_frame_duration;
}

SDL_cond *demuxer_get_continue_read_thread(const Demuxer *demuxer)
{
    if (!demuxer)
        return NULL;
    return demuxer->continue_read_thread;
}

int demuxer_get_queue_attachments_req(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->queue_attachments_req;
}

void demuxer_set_queue_attachments_req(Demuxer *demuxer, int req)
{
    if (!demuxer)
        return;
    demuxer->queue_attachments_req = req;
}

int demuxer_get_read_pause_return(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->read_pause_return;
}

void demuxer_set_read_pause_return(Demuxer *demuxer, int ret)
{
    if (!demuxer)
        return;
    demuxer->read_pause_return = ret;
}

int demuxer_open_input(Demuxer *demuxer, AVDictionary **options)
{
    AVDictionary *open_opts = NULL;
    if (!av_dict_get(open_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE))
        av_dict_set(&open_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    int err = avformat_open_input(&demuxer->ic, demuxer_get_input_name(demuxer), NULL, &open_opts);
    av_dict_free(&open_opts);
    open_opts = NULL;
    if (err < 0) {
        print_error(demuxer_get_input_name(demuxer), err);
        return -1;
    }
    return 0;
}

int demuxer_find_stream_info(Demuxer *demuxer, AVDictionary **options)
{
    int err = avformat_find_stream_info(demuxer->ic, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "%s: could not find codec parameters\n", demuxer_get_input_name(demuxer));
        return -1;
    }
    return 0;
}

void demuxer_io_reset_eof(Demuxer *demuxer)
{
    // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
    if (demuxer->ic->pb)
        demuxer->ic->pb->eof_reached = 0;
}

int demuxer_should_use_byte_seek(Demuxer* demuxer)
{
    if (!demuxer || !demuxer->ic || !demuxer->ic->iformat)
        return 0;

    AVFormatContext *ic = demuxer->ic;

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

double demuxer_get_max_gap(Demuxer* demuxer)
{
    if (!demuxer || !demuxer->ic || !demuxer->ic->iformat)
        return 3600.0;

    AVFormatContext *ic = demuxer->ic;
    if (ic->iformat->flags & AVFMT_TS_DISCONT)
        return 10.0;   // Unstable timestamps: be strict
    return 3600.0;     // Stable timestamps: be lenient
}

int demuxer_is_realtime(Demuxer *demuxer)
{
    if (!demuxer)
        return -1;

    AVFormatContext *s = demuxer->ic;
    if (!s)
        return -1;

    if (s->iformat && s->iformat->name &&
        (!strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp"))) {
        return 1;
    }

    if (s->pb && (!strncmp(s->url, "rtp:", 4)
        || !strncmp(s->url, "udp:", 4))) {
        return 1;
    }
    return 0;
}

int demuxer_find_stream_components(Demuxer *demuxer)
{
    if (!demuxer->ic) {
        av_log(NULL, AV_LOG_ERROR, "AVFormatContext not initialized\n");
        return -1;
    }

    for (int i = 0; i < demuxer->ic->nb_streams; i++) {
        AVStream *st = demuxer->ic->streams[i];
        st->discard = AVDISCARD_ALL;
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }

    demuxer->st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(demuxer->ic, AVMEDIA_TYPE_VIDEO,
                            demuxer->st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    demuxer->st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(demuxer->ic, AVMEDIA_TYPE_AUDIO,
                            demuxer->st_index[AVMEDIA_TYPE_AUDIO],
                            demuxer->st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);
    demuxer->st_index[AVMEDIA_TYPE_SUBTITLE] =
        av_find_best_stream(demuxer->ic, AVMEDIA_TYPE_SUBTITLE,
                            demuxer->st_index[AVMEDIA_TYPE_SUBTITLE],
                            (demuxer->st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                             demuxer->st_index[AVMEDIA_TYPE_AUDIO] :
                             demuxer->st_index[AVMEDIA_TYPE_VIDEO]),
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
    AVFormatContext *ic = d->ic;
    return ic && ic->pb && ic->pb->error;
}

int demuxer_should_handle_eof(Demuxer *d, int ret)
{
    AVFormatContext *ic = d->ic;
    if (!ic)
        return 0;
    if (ret == AVERROR_EOF || (ic->pb && avio_feof(ic->pb))) {
        return !demuxer_is_eof(d);
    }
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

int demuxer_start(Demuxer *demuxer, int (*read_thread_fn)(void *), void *arg)
{
    if (!demuxer || !read_thread_fn)
        return AVERROR(EINVAL);
    demuxer->read_tid = SDL_CreateThread(read_thread_fn, "read_thread", arg);
    if (!demuxer->read_tid)
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

void demuxer_stop(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    if (demuxer->read_tid) {
        SDL_WaitThread(demuxer->read_tid, NULL);
        demuxer->read_tid = NULL;
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
