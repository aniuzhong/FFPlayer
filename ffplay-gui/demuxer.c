#include <errno.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/mem.h>

#include "demuxer.h"

int demuxer_init(Demuxer *demuxer, const char *input_url)
{
    if (!demuxer || !input_url)
        return AVERROR(EINVAL);
    memset(demuxer, 0, sizeof(*demuxer));
    demuxer->seek_mode = -1;
    demuxer->realtime = 0;
    demuxer->eof = 0;
    demuxer->max_frame_duration = 0.0;
    demuxer->input_url = av_strdup(input_url);
    if (!demuxer->input_url)
        return AVERROR(ENOMEM);
    if (!(demuxer->continue_read_thread = SDL_CreateCond())) {
        av_freep(&demuxer->input_url);
        return AVERROR(ENOMEM);
    }
    return 0;
}

void demuxer_destroy(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    avformat_close_input(&demuxer->ic);
    av_freep(&demuxer->input_url);
    if (demuxer->continue_read_thread) {
        SDL_DestroyCond(demuxer->continue_read_thread);
        demuxer->continue_read_thread = NULL;
    }
    demuxer->seek_mode = -1;
    demuxer->abort_request = 0;
    demuxer->realtime = 0;
    demuxer->eof = 0;
    demuxer->max_frame_duration = 0.0;
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

void demuxer_stop(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    if (demuxer->read_tid) {
        SDL_WaitThread(demuxer->read_tid, NULL);
        demuxer->read_tid = NULL;
    }
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

const char *demuxer_get_input_name(const Demuxer *demuxer)
{
    if (!demuxer || !demuxer->input_url)
        return "";
    return demuxer->input_url;
}

int demuxer_is_realtime(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->realtime;
}

int demuxer_is_eof(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->eof;
}

double demuxer_get_max_frame_duration(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0.0;
    return demuxer->max_frame_duration;
}

void demuxer_set_realtime(Demuxer *demuxer, int realtime)
{
    if (!demuxer)
        return;
    demuxer->realtime = realtime;
}

void demuxer_set_eof(Demuxer *demuxer, int eof)
{
    if (!demuxer)
        return;
    demuxer->eof = eof;
}

void demuxer_set_max_frame_duration(Demuxer *demuxer, double max_frame_duration)
{
    if (!demuxer)
        return;
    demuxer->max_frame_duration = max_frame_duration;
}

