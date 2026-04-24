#ifndef FFPLAY_GUI_STREAM_H
#define FFPLAY_GUI_STREAM_H

#include <stdint.h>
#include <SDL.h>
#include <libavutil/rational.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoState VideoState;
typedef struct AudioDevice AudioDevice;
typedef struct Demuxer Demuxer;
typedef struct PacketQueue PacketQueue;
struct AVStream;
struct AVFrame;
struct AVSubtitle;

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes);
void stream_toggle_pause(VideoState *is);
void stream_toggle_pause_and_clear_step(VideoState *is);
void stream_toggle_mute(VideoState *is);
void stream_toggle_audio_display(VideoState *is);
void stream_set_volume(VideoState *is, int volume);
void stream_refresh(VideoState *is, double *remaining_time);
void stream_request_refresh(VideoState *is);
void stream_handle_window_size_changed(VideoState *is, int width, int height);
void stream_adjust_volume_step(VideoState *is, int sign, double step);
void stream_step(VideoState *is);
void stream_seek_chapter(VideoState *is, int incr);
void stream_seek_relative(VideoState *is, double incr_seconds);
double stream_get_master_clock(VideoState *is);
VideoState *stream_open(const char *filename,
                        AudioDevice *audio_device,
                        const SDL_RendererInfo *renderer_info,
                        void (*frame_size_changed_cb)(void *opaque, int width, int height, AVRational sar),
                        void *frame_size_opaque);
int stream_has_enough_packets(struct AVStream *st, int stream_id, PacketQueue *queue);

int stream_component_open(VideoState *is, int stream_index);
void stream_component_close(VideoState *is, int stream_index);
void stream_close(VideoState *is);
void stream_cycle_channel(VideoState *is, int codec_type);

/* -- Accessors (opaque-friendly) --------------- */
Demuxer    *stream_get_demuxer(const VideoState *is);
int         stream_is_paused(const VideoState *is);
int         stream_get_volume(const VideoState *is);
int         stream_needs_refresh(const VideoState *is);
int         stream_is_video_open(const VideoState *is);

/* -- High-level queries ------------------------ */
void        stream_seek_to_ratio(VideoState *is, float ratio);
double      stream_get_position(const VideoState *is);
double      stream_get_duration(const VideoState *is);
int         stream_is_eof(const VideoState *is);
int         stream_has_quit_request(const VideoState *is);
int         stream_has_chapters(const VideoState *is);
const char *stream_get_media_title(const VideoState *is);
int         stream_can_seek(const VideoState *is);
float       stream_get_byte_progress(const VideoState *is);

void        stream_cycle_audio(VideoState *is);
void        stream_cycle_video(VideoState *is);
void        stream_cycle_subtitle(VideoState *is);

/* -- Frame access (pull-based) ---------------- */
struct AudioVisualizer *stream_get_audio_visualizer(const VideoState *is);
struct AVFrame    *stream_get_video_frame(const VideoState *is);
struct AVSubtitle *stream_get_subtitle(const VideoState *is);
int                stream_get_video_size(const VideoState *is, int *width, int *height, AVRational *sar);
int                stream_get_show_mode(const VideoState *is);
void               stream_set_window_size(VideoState *is, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
