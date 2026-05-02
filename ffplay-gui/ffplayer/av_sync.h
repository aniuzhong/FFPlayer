/*
 * AV sync implementation.
 *
 * To match ffplay src/ffplay.c master-clock selection, AVSync must satisfy:
 * - The members in AVSync are borrowed references;
 * - @c av_sync_type points at the session's requested mode (ffplay @c -sync);
 *   @c get_master_sync_type applies stream fallbacks (missing video/audio);
 */

#ifndef FFPLAY_GUI_AV_SYNC_H
#define FFPLAY_GUI_AV_SYNC_H

#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
#define AV_NOSYNC_THRESHOLD 10.0

#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#include "clock.h"
#include "frame_queue.h"

typedef struct AVStream AVStream;
typedef struct AVSync {
    Clock       *audclk;
    Clock       *vidclk;
    Clock       *extclk;
    PacketQueue *audioq;
    PacketQueue *videoq;
    AVStream    **audio_st;
    AVStream    **video_st;
    int         *audio_stream;
    int         *video_stream;
    double      *max_frame_duration;
    int         *av_sync_type;
} AVSync;

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK,
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Context binding
 */
void av_sync_bind(AVSync *sync, Clock *audclk, Clock *vidclk, Clock *extclk,
                  PacketQueue *audioq, PacketQueue *videoq, AVStream **audio_st,
                  AVStream **video_st, int *audio_stream, int *video_stream,
                  double *max_frame_duration, int *av_sync_type);

/**
 * Master-clock query
 */
int get_master_sync_type(const AVSync *sync);
int av_sync_is_audio_master(const AVSync *sync);
double get_master_clock(const AVSync *sync);

/*
 * Sync control and delay calculation
 */
void check_external_clock_speed(AVSync *sync);
double av_sync_compute_frame_delay(const AVSync *sync, Frame *lastvp, Frame *vp);
double compute_target_delay(double delay, const AVSync *sync);
double vp_duration(const AVSync *sync, Frame *vp, Frame *nextvp);

/**
 * Clock update helpers
 */
void update_video_pts(AVSync *sync, double pts, int serial);
void av_sync_update_video_pts_if_valid(AVSync *sync, double pts, int serial);
void av_sync_sync_extclk_to_audclk(AVSync *sync);
void av_sync_seek_reset_extclk(AVSync *sync, int by_bytes, int64_t seek_target);
void av_sync_update_audclk_from_callback(AVSync *sync, double audio_clock, int audio_clock_serial,
                                         int audio_hw_buf_size, int audio_write_buf_size, int audio_tgt_bytes_per_sec,
                                         int64_t audio_callback_time);
void av_sync_toggle_pause(AVSync *sync, int *paused, double *frame_timer, int read_pause_return);

/**
 * Decision helpers
 */
double av_sync_audio_master_diff(const AVSync *sync);
double av_sync_video_master_diff(const AVSync *sync, double video_clock);
int av_sync_is_external_clock_master(const AVSync *sync);
int av_sync_should_late_drop(const AVSync *sync, int step, double time, double frame_timer, double duration);
int av_sync_should_reset_frame_timer(double delay, double time, double frame_timer);
int av_sync_should_early_drop(const AVSync *sync, double video_clock, double frame_last_filter_delay,
                              int video_pkt_serial, int video_clock_serial, int video_queue_nb_packets);
int av_sync_should_clear_frame_filter_delay(double frame_last_filter_delay);

#ifdef __cplusplus
}
#endif

#endif
