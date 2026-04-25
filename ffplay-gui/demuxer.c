#include <errno.h>
#include <string.h>

#include <SDL_thread.h>

#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "demuxer.h"

typedef struct Demuxer {
    AVFormatContext *ic;
    int              seek_mode;
    int              abort_request;
    int              realtime;
    int              eof;
    double           max_frame_duration;
    char            *input_url;
    SDL_Thread      *read_tid;
    SDL_cond        *continue_read_thread;
    int              queue_attachments_req;
    int              read_pause_return;
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

    demuxer->seek_mode          = -1;
    demuxer->realtime           = 0;
    demuxer->eof                = 0;
    demuxer->max_frame_duration = 0.0;

    demuxer->input_url = av_strdup(input_url);
    if (!demuxer->input_url)
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
        if (demuxer->continue_read_thread) {
            SDL_DestroyCond(demuxer->continue_read_thread);
        }
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
    if (d->continue_read_thread) {
        SDL_DestroyCond(d->continue_read_thread);
        d->continue_read_thread = NULL;
    }
    av_free(d);
    *demuxer = NULL;
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

int demuxer_start(Demuxer *demuxer, int (*read_thread_fn)(void *), void *arg)
{
    if (!demuxer || !read_thread_fn)
        return AVERROR(EINVAL);
    demuxer->read_tid = SDL_CreateThread(read_thread_fn, "read_thread", arg);
    if (!demuxer->read_tid)
        return AVERROR(ENOMEM);
    return 0;
}

/**
 * Stop the read thread.
 */
void demuxer_stop(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    if (demuxer->read_tid) {
        SDL_WaitThread(demuxer->read_tid, NULL);
        demuxer->read_tid = NULL;
    }
}

/**
 * Signal the read thread to continue.
 */
void demuxer_notify_continue_read(Demuxer *demuxer)
{
    if (demuxer && demuxer->continue_read_thread) {
        SDL_CondSignal(demuxer->continue_read_thread);
    }
}

/**
 * Request abort of the demuxer.
 */
void demuxer_request_abort(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    demuxer->abort_request = 1;
}

/**
 * Check if abort is requested.
 */
int demuxer_is_aborted(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->abort_request;
}

/**
 * Get seek mode.
 */
int demuxer_get_seek_mode(const Demuxer *demuxer)
{
    if (!demuxer || demuxer->seek_mode < 0)
        return 0;
    return demuxer->seek_mode;
}

/**
 * Set seek mode.
 */
void demuxer_set_seek_mode(Demuxer *demuxer, int seek_mode)
{
    if (!demuxer)
        return;
    demuxer->seek_mode = seek_mode;
}

/**
 * Get input URL.
 */
const char *demuxer_get_input_name(const Demuxer *demuxer)
{
    if (!demuxer || !demuxer->input_url)
        return "";
    return demuxer->input_url;
}

/**
 * Get AVFormatContext.
 */
AVFormatContext *demuxer_get_format_context(const Demuxer *demuxer)
{
    /* TODO Consider a better wrapper instead of direct access to demuxer->ic */
    if (!demuxer)
        return NULL;
    return demuxer->ic;
}

/**
 * Set AVFormatContext.
 */
void demuxer_set_ic(Demuxer *demuxer, AVFormatContext *ic)
{
    if (!demuxer)
        return;
    demuxer->ic = ic;
}

/**
 * Check if realtime format.
 */
int demuxer_is_realtime(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->realtime;
}

/**
 * Set realtime flag.
 */
void demuxer_set_realtime(Demuxer *demuxer, int realtime)
{
    if (!demuxer)
        return;
    demuxer->realtime = realtime;
}

/**
 * Check if EOF reached.
 */
int demuxer_is_eof(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->eof;
}

/**
 * Set EOF flag.
 */
void demuxer_set_eof(Demuxer *demuxer, int eof)
{
    if (!demuxer)
        return;
    demuxer->eof = eof;
}

/**
 * Get max frame duration.
 */
double demuxer_get_max_frame_duration(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0.0;
    return demuxer->max_frame_duration;
}

/**
 * Set max frame duration.
 */
void demuxer_set_max_frame_duration(Demuxer *demuxer, double max_frame_duration)
{
    if (!demuxer)
        return;
    demuxer->max_frame_duration = max_frame_duration;
}

/**
 * Get pointer to max frame duration (for av_sync_bind).
 */
double *demuxer_get_max_frame_duration_ptr(Demuxer *demuxer)
{
    if (!demuxer)
        return NULL;
    return &demuxer->max_frame_duration;
}

/**
 * Get continue read thread condition.
 */
SDL_cond *demuxer_get_continue_read_thread(const Demuxer *demuxer)
{
    if (!demuxer)
        return NULL;
    return demuxer->continue_read_thread;
}

/**
 * Get queue attachments request flag.
 */
int demuxer_get_queue_attachments_req(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->queue_attachments_req;
}

/**
 * Set queue attachments request flag.
 */
void demuxer_set_queue_attachments_req(Demuxer *demuxer, int req)
{
    if (!demuxer)
        return;
    demuxer->queue_attachments_req = req;
}

/**
 * Get read pause return value.
 */
int demuxer_get_read_pause_return(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->read_pause_return;
}

/**
 * Set read pause return value.
 */
void demuxer_set_read_pause_return(Demuxer *demuxer, int ret)
{
    if (!demuxer)
        return;
    demuxer->read_pause_return = ret;
}

/**
 * Check if format is realtime.
 */
int is_realtime(AVFormatContext *s)
{
    if (!strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp")) {
        return 1;
    }

    if (s->pb && (!strncmp(s->url, "rtp:", 4)
        || !strncmp(s->url, "udp:", 4))) {
        return 1;
    }
    return 0;
}

