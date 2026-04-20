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
void stream_adjust_volume_step(VideoState *is, int sign, double step);
void stream_step(VideoState *is);
void stream_seek_chapter(VideoState *is, int incr);
void stream_seek_relative(VideoState *is, double incr_seconds);
VideoState *stream_open(const char *filename,
                        AudioDevice *audio_device,
                        VideoRenderer *video_renderer,
                        void (*frame_size_changed_cb)(VideoState *is, int width, int height, AVRational sar));
int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue);
int is_realtime(AVFormatContext *s);
int stream_component_open(VideoState *is, int stream_index);
void stream_component_close(VideoState *is, int stream_index);
void stream_close(VideoState *is);
void stream_cycle_channel(VideoState *is, int codec_type);

#ifdef __cplusplus
}
#endif

#endif
