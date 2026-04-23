#ifndef FFPLAY_GUI_STREAM_H
#define FFPLAY_GUI_STREAM_H

#include "video_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes);
void stream_toggle_pause(VideoState *is);
void stream_toggle_pause_and_clear_step(VideoState *is);
void stream_toggle_mute(VideoState *is);
void stream_toggle_audio_display(VideoState *is);
void stream_set_volume(VideoState *is, int volume);
void stream_refresh(VideoState *is, double *remaining_time);
void stream_display(VideoState *is, VideoRenderer *vr);
void stream_request_refresh(VideoState *is);
void stream_handle_window_size_changed(VideoState *is, int width, int height);
void stream_adjust_volume_step(VideoState *is, int sign, double step);
void stream_step(VideoState *is);
void stream_seek_chapter(VideoState *is, int incr);
void stream_seek_relative(VideoState *is, double incr_seconds);
double stream_get_master_clock(VideoState *is);
VideoState *stream_open(const char *filename,
                        AudioDevice *audio_device,
                        VideoRenderer *video_renderer,
                        void (*frame_size_changed_cb)(VideoState *is, int width, int height, AVRational sar));
int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue);

int stream_component_open(VideoState *is, int stream_index);
void stream_component_close(VideoState *is, int stream_index);
void stream_close(VideoState *is);
void stream_cycle_channel(VideoState *is, int codec_type);

/* ── Accessors (opaque-friendly) ─────────────── */
Demuxer    *stream_get_demuxer(const VideoState *is);
int         stream_is_paused(const VideoState *is);
int         stream_get_volume(const VideoState *is);
int         stream_needs_refresh(const VideoState *is);
int         stream_is_video_open(const VideoState *is);

#ifdef __cplusplus
}
#endif

#endif
