#include <math.h>

#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavformat/avio.h>

#include "audio_pipeline.h"
#include "audio_visualizer.h"
#include "frame_queue.h"
#include "demuxer.h"
#include "stream.h"
#include "video_renderer.h"
#include "ffplayer.h"

struct FFPlayer {
    AudioDevice   *audio_device;
    VideoRenderer *video_renderer;
    VideoState    *is;
};

/* ── Lifecycle ────────────────────────────────── */

FFPlayer *ffplayer_create(AudioDevice *audio_device, VideoRenderer *video_renderer)
{
    FFPlayer *p;
    if (!audio_device || !video_renderer)
        return NULL;
    p = av_mallocz(sizeof(FFPlayer));
    if (!p)
        return NULL;
    p->audio_device  = audio_device;
    p->video_renderer = video_renderer;
    p->is = NULL;
    audio_device_set_open_cb(audio_device, audio_pipeline_open);
    return p;
}

void ffplayer_free(FFPlayer **pp)
{
    FFPlayer *p;
    if (!pp || !*pp)
        return;
    p = *pp;
    ffplayer_close(p);
    av_freep(pp);
}

/* ── Open / Close media ───────────────────────── */

int ffplayer_open(FFPlayer *p, const char *url)
{
    if (!p || !url)
        return -1;
    if (p->is)
        ffplayer_close(p);
    p->is = stream_open(url, p->audio_device, p->video_renderer, NULL);
    return p->is ? 0 : -1;
}

void ffplayer_close(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_close(p->is);
    p->is = NULL;
}

int ffplayer_is_open(const FFPlayer *p)
{
    return p && p->is != NULL;
}

/* ── Playback control ─────────────────────────── */

void ffplayer_toggle_pause(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_toggle_pause_and_clear_step(p->is);
}

int ffplayer_is_paused(const FFPlayer *p)
{
    if (!p || !p->is)
        return 1;
    return p->is->paused;
}

void ffplayer_step_frame(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_step(p->is);
}

/* ── Seek ─────────────────────────────────────── */

void ffplayer_seek_relative(FFPlayer *p, double incr_seconds)
{
    if (!p || !p->is)
        return;
    stream_seek_relative(p->is, incr_seconds);
}

void ffplayer_seek_to_ratio(FFPlayer *p, float ratio)
{
    int64_t ts;
    int64_t size;
    AVFormatContext *ic;
    if (!p || !p->is)
        return;
    ic = demuxer_get_ic(p->is->demuxer);
    if (!ic)
        return;
    ratio = av_clipf(ratio, 0.0f, 1.0f);
    if (demuxer_get_seek_mode(p->is->demuxer) || ic->duration <= 0) {
        if (!ic->pb)
            return;
        size = avio_size(ic->pb);
        if (size <= 0)
            return;
        stream_seek(p->is, (int64_t)(size * ratio), 0, 1);
        return;
    }
    ts = (int64_t)(ratio * ic->duration);
    if (ic->start_time != AV_NOPTS_VALUE)
        ts += ic->start_time;
    stream_seek(p->is, ts, 0, 0);
}

void ffplayer_seek_chapter(FFPlayer *p, int incr)
{
    if (!p || !p->is)
        return;
    stream_seek_chapter(p->is, incr);
}

/* ── Audio ────────────────────────────────────── */

void ffplayer_set_volume(FFPlayer *p, int volume)
{
    if (!p || !p->is)
        return;
    stream_set_volume(p->is, volume);
}

int ffplayer_get_volume(const FFPlayer *p)
{
    if (!p || !p->is || !p->is->audio_pipeline)
        return 0;
    return p->is->audio_pipeline->audio_volume;
}

void ffplayer_adjust_volume_step(FFPlayer *p, int sign, double step)
{
    if (!p || !p->is)
        return;
    stream_adjust_volume_step(p->is, sign, step);
}

void ffplayer_toggle_mute(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_toggle_mute(p->is);
}

/* ── Track selection ──────────────────────────── */

void ffplayer_cycle_audio_track(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_channel(p->is, AVMEDIA_TYPE_AUDIO);
}

void ffplayer_cycle_video_track(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_channel(p->is, AVMEDIA_TYPE_VIDEO);
}

void ffplayer_cycle_subtitle_track(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_channel(p->is, AVMEDIA_TYPE_SUBTITLE);
}

void ffplayer_cycle_all_tracks(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_channel(p->is, AVMEDIA_TYPE_VIDEO);
    stream_cycle_channel(p->is, AVMEDIA_TYPE_AUDIO);
    stream_cycle_channel(p->is, AVMEDIA_TYPE_SUBTITLE);
}

/* ── Display mode ─────────────────────────────── */

void ffplayer_toggle_audio_display(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_toggle_audio_display(p->is);
}

/* ── Media info ───────────────────────────────── */

double ffplayer_get_position(const FFPlayer *p)
{
    double pos;
    AVFormatContext *ic;
    if (!p || !p->is)
        return 0.0;
    pos = stream_get_master_clock(p->is);
    if (isnan(pos))
        return 0.0;
    ic = demuxer_get_ic(p->is->demuxer);
    if (ic && ic->start_time != AV_NOPTS_VALUE)
        pos -= ic->start_time / (double)AV_TIME_BASE;
    return pos > 0.0 ? pos : 0.0;
}

double ffplayer_get_duration(const FFPlayer *p)
{
    AVFormatContext *ic;
    if (!p || !p->is)
        return -1.0;
    ic = demuxer_get_ic(p->is->demuxer);
    if (!ic || ic->duration <= 0)
        return -1.0;
    return ic->duration / (double)AV_TIME_BASE;
}

int ffplayer_is_eof(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return demuxer_is_eof(p->is->demuxer);
}

int ffplayer_has_chapters(const FFPlayer *p)
{
    AVFormatContext *ic;
    if (!p || !p->is)
        return 0;
    ic = demuxer_get_ic(p->is->demuxer);
    return ic && ic->nb_chapters > 1;
}

const char *ffplayer_get_media_title(const FFPlayer *p)
{
    if (!p || !p->is)
        return NULL;
    return demuxer_get_input_name(p->is->demuxer);
}

int ffplayer_can_seek(const FFPlayer *p)
{
    AVFormatContext *ic;
    if (!p || !p->is)
        return 0;
    ic = demuxer_get_ic(p->is->demuxer);
    if (!ic)
        return 0;
    if (ic->duration > 0)
        return 1;
    return ic->pb && avio_size(ic->pb) > 0;
}

float ffplayer_get_byte_progress(const FFPlayer *p)
{
    AVFormatContext *ic;
    int64_t size, pos;
    if (!p || !p->is)
        return -1.0f;
    ic = demuxer_get_ic(p->is->demuxer);
    if (!ic || !ic->pb)
        return -1.0f;
    size = avio_size(ic->pb);
    pos = avio_tell(ic->pb);
    if (size > 0 && pos >= 0)
        return av_clipf((float)pos / (float)size, 0.0f, 1.0f);
    return -1.0f;
}

/* ── Render loop integration ──────────────────── */

int ffplayer_needs_refresh(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return p->is->show_mode != SHOW_MODE_NONE &&
           (!p->is->paused || p->is->force_refresh);
}

void ffplayer_refresh(FFPlayer *p, double *remaining_time)
{
    if (!p || !p->is)
        return;
    stream_refresh(p->is, remaining_time);
}

void ffplayer_display(FFPlayer *p)
{
    VideoState *is;
    VideoRenderer *vr;
    if (!p || !p->is)
        return;
    is = p->is;
    vr = p->video_renderer;

    if (!is->width) {
        video_renderer_open(vr, &is->width, &is->height);
        if (is->on_video_open)
            is->on_video_open(is);
    }

    video_renderer_clear(vr);
    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
        audio_visualizer_render(is->audio_visualizer, vr->renderer, is->xleft, is->ytop, is->width, is->height);
    else if (is->video_st) {
        Frame *vp = frame_queue_peek_last(is->pictq);
        Frame *sp = (is->subtitle_st && frame_queue_nb_remaining(is->subpq) > 0) ? frame_queue_peek(is->subpq) : NULL;
        video_renderer_draw_video(vr, vp, sp, is->xleft, is->ytop, is->width, is->height);
    }
}

/* ── Window events ────────────────────────────── */

void ffplayer_handle_window_size_changed(FFPlayer *p, int width, int height)
{
    if (!p || !p->is)
        return;
    stream_handle_window_size_changed(p->is, width, height);
}

void ffplayer_request_refresh(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_request_refresh(p->is);
}

int ffplayer_is_renderer_open(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return p->is->width > 0;
}
