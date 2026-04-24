#ifndef FFPLAY_GUI_FFPLAYER_H
#define FFPLAY_GUI_FFPLAYER_H

#include <stdint.h>
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioDevice AudioDevice;
typedef struct VideoRenderer VideoRenderer;
typedef struct FFPlayer FFPlayer;

#define FFPLAYER_VOLUME_STEP     (0.75)
#define FFPLAYER_REFRESH_RATE    0.01
#define FFPLAYER_CURSOR_HIDE_DELAY 1000000
#define FFPLAYER_QUIT_EVENT      (SDL_USEREVENT + 2)

/* -- Lifecycle ---------------------------------- */

FFPlayer *ffplayer_create(AudioDevice *audio_device, VideoRenderer *video_renderer);
void      ffplayer_free(FFPlayer **p);

/* -- Open / Close media ------------------------- */

int  ffplayer_open(FFPlayer *p, const char *url);
void ffplayer_close(FFPlayer *p);
int  ffplayer_is_open(const FFPlayer *p);

/* -- Playback control --------------------------- */

void ffplayer_toggle_pause(FFPlayer *p);
int  ffplayer_is_paused(const FFPlayer *p);
void ffplayer_step_frame(FFPlayer *p);

/* -- Seek --------------------------------------- */

void ffplayer_seek_relative(FFPlayer *p, double incr_seconds);
void ffplayer_seek_to_ratio(FFPlayer *p, float ratio);
void ffplayer_seek_chapter(FFPlayer *p, int incr);

/* -- Audio -------------------------------------- */

void ffplayer_set_volume(FFPlayer *p, int volume);
int  ffplayer_get_volume(const FFPlayer *p);
void ffplayer_adjust_volume_step(FFPlayer *p, int sign, double step);
void ffplayer_toggle_mute(FFPlayer *p);

/* -- Track selection ---------------------------- */

void ffplayer_cycle_audio_track(FFPlayer *p);
void ffplayer_cycle_video_track(FFPlayer *p);
void ffplayer_cycle_subtitle_track(FFPlayer *p);
void ffplayer_cycle_all_tracks(FFPlayer *p);

/* -- Display mode ------------------------------- */

void ffplayer_toggle_audio_display(FFPlayer *p);

/* -- Media info (read-only queries) ------------- */

double ffplayer_get_position(const FFPlayer *p);
double ffplayer_get_duration(const FFPlayer *p);
int    ffplayer_is_eof(const FFPlayer *p);
int    ffplayer_has_chapters(const FFPlayer *p);
const char *ffplayer_get_media_title(const FFPlayer *p);
int    ffplayer_can_seek(const FFPlayer *p);
float  ffplayer_get_byte_progress(const FFPlayer *p);

/* -- Render loop integration -------------------- */

int  ffplayer_needs_refresh(const FFPlayer *p);
void ffplayer_refresh(FFPlayer *p, double *remaining_time);
void ffplayer_display(FFPlayer *p);

/* -- Window events (forwarded by application) --- */

void ffplayer_handle_window_size_changed(FFPlayer *p, int width, int height);
void ffplayer_request_refresh(FFPlayer *p);
int  ffplayer_is_renderer_open(const FFPlayer *p);

#ifdef __cplusplus
}
#endif

#endif
