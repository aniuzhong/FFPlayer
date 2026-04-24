#ifndef FFPLAY_GUI_FFPLAYER_H
#define FFPLAY_GUI_FFPLAYER_H

#include <stdint.h>

#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioDevice AudioDevice;
typedef struct AudioVisualizer AudioVisualizer;
typedef struct FFPlayer FFPlayer;

enum FFPlayerShowMode {
    FFPLAYER_SHOW_MODE_NONE = -1,
    FFPLAYER_SHOW_MODE_VIDEO = 0,
    FFPLAYER_SHOW_MODE_WAVES,
    FFPLAYER_SHOW_MODE_RDFT,
    FFPLAYER_SHOW_MODE_NB
};

#define FFPLAYER_VOLUME_STEP     (0.75)
#define FFPLAYER_REFRESH_RATE    0.01
#define FFPLAYER_CURSOR_HIDE_DELAY 1000000

/* -- Lifecycle ---------------------------------- */

FFPlayer *ffplayer_create(AudioDevice *audio_device);
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

/* -- Frame access (pull-based) ------------------ */

/**
 * Get the current video frame selected by the sync algorithm.
 * Returns a borrowed AVFrame pointer (do NOT free), or NULL.
 * Valid until the next call to ffplayer_refresh().
 */
AVFrame *ffplayer_get_video_frame(const FFPlayer *p);

/**
 * Get the current subtitle that should be overlaid.
 * Returns a borrowed AVSubtitle pointer, or NULL.
 * Valid until the next call to ffplayer_refresh().
 */
AVSubtitle *ffplayer_get_subtitle(const FFPlayer *p);

/**
 * Get the video display dimensions and sample aspect ratio.
 * Returns 0 on success, -1 if no video.
 */
int ffplayer_get_video_size(const FFPlayer *p, int *width, int *height, AVRational *sar);

/**
 * Get the current display mode.
 */
enum FFPlayerShowMode ffplayer_get_show_mode(const FFPlayer *p);

/**
 * Get the audio visualizer handle (for rendering audio waveform/spectrum).
 * Returns NULL if not available. The caller provides its own renderer.
 */
AudioVisualizer *ffplayer_get_audio_visualizer(const FFPlayer *p);

/* -- Window events (forwarded by application) --- */

void ffplayer_set_window_size(FFPlayer *p, int width, int height);
void ffplayer_handle_window_size_changed(FFPlayer *p, int width, int height);
void ffplayer_request_refresh(FFPlayer *p);
int  ffplayer_is_video_open(const FFPlayer *p);

/**
 * Set renderer info for video filter pixel format selection.
 * Pass a pointer to SDL_RendererInfo (cast to void*).
 * Must be called before ffplayer_open().
 */
void ffplayer_set_renderer_info(FFPlayer *p, const void *renderer_info);

/**
 * Callback: invoked from the video thread when the coded frame size changes.
 * The application can use this to adjust window / default size.
 */
typedef void (*ffplayer_frame_size_cb)(void *opaque, int width, int height, AVRational sar);
void ffplayer_set_frame_size_callback(FFPlayer *p, ffplayer_frame_size_cb cb, void *opaque);

#ifdef __cplusplus
}
#endif

#endif
