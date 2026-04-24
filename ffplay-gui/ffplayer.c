#include <libavutil/mem.h>

#include "stream.h"
#include "video_renderer.h"
#include "ffplayer.h"

struct FFPlayer {
    AudioDevice   *audio_device;
    VideoRenderer *video_renderer;
    VideoState    *is;
};

/* -- Lifecycle ---------------------------------- */

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

/* -- Open / Close media ------------------------- */

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

/* -- Playback control --------------------------- */

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
    return stream_is_paused(p->is);
}

void ffplayer_step_frame(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_step(p->is);
}

/* -- Seek --------------------------------------- */

void ffplayer_seek_relative(FFPlayer *p, double incr_seconds)
{
    if (!p || !p->is)
        return;
    stream_seek_relative(p->is, incr_seconds);
}

void ffplayer_seek_to_ratio(FFPlayer *p, float ratio)
{
    if (!p || !p->is)
        return;
    stream_seek_to_ratio(p->is, ratio);
}

void ffplayer_seek_chapter(FFPlayer *p, int incr)
{
    if (!p || !p->is)
        return;
    stream_seek_chapter(p->is, incr);
}

/* -- Audio -------------------------------------- */

void ffplayer_set_volume(FFPlayer *p, int volume)
{
    if (!p || !p->is)
        return;
    stream_set_volume(p->is, volume);
}

int ffplayer_get_volume(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return stream_get_volume(p->is);
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

/* -- Track selection ---------------------------- */

void ffplayer_cycle_audio_track(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_audio(p->is);
}

void ffplayer_cycle_video_track(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_video(p->is);
}

void ffplayer_cycle_subtitle_track(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_subtitle(p->is);
}

void ffplayer_cycle_all_tracks(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_cycle_video(p->is);
    stream_cycle_audio(p->is);
    stream_cycle_subtitle(p->is);
}

/* -- Display mode ------------------------------- */

void ffplayer_toggle_audio_display(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_toggle_audio_display(p->is);
}

/* -- Media info --------------------------------- */

double ffplayer_get_position(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0.0;
    return stream_get_position(p->is);
}

double ffplayer_get_duration(const FFPlayer *p)
{
    if (!p || !p->is)
        return -1.0;
    return stream_get_duration(p->is);
}

int ffplayer_is_eof(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return stream_is_eof(p->is);
}

int ffplayer_has_chapters(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return stream_has_chapters(p->is);
}

const char *ffplayer_get_media_title(const FFPlayer *p)
{
    if (!p || !p->is)
        return NULL;
    return stream_get_media_title(p->is);
}

int ffplayer_can_seek(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return stream_can_seek(p->is);
}

float ffplayer_get_byte_progress(const FFPlayer *p)
{
    if (!p || !p->is)
        return -1.0f;
    return stream_get_byte_progress(p->is);
}

/* -- Render loop integration -------------------- */

int ffplayer_needs_refresh(const FFPlayer *p)
{
    if (!p || !p->is)
        return 0;
    return stream_needs_refresh(p->is);
}

void ffplayer_refresh(FFPlayer *p, double *remaining_time)
{
    if (!p || !p->is)
        return;
    stream_refresh(p->is, remaining_time);
}

void ffplayer_display(FFPlayer *p)
{
    if (!p || !p->is)
        return;
    stream_display(p->is, p->video_renderer);
}

/* -- Window events ------------------------------ */

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
    return stream_is_video_open(p->is);
}
