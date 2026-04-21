/*
 * AV sync implementation.
 *
 * To be fully equivalent to the behavior of src/ffplay.c, AvSync must satisfy:
 * - The members in AvSync are borrowed references;
 *   they do not perform ownership management and do not maintain an independent state machine;
 * - Each computation reads the current clocks, the current queue packet count/serial,
 *   and the current stream availability;
 * - Without altering the original call order or thread boundaries;
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
typedef struct AvSync {
    Clock       *audclk;
    Clock       *vidclk;
    Clock       *extclk;
    PacketQueue *audioq;
    PacketQueue *videoq;
    AVStream    **audio_st;
    int         *audio_stream;
    int         *video_stream;
    double      *max_frame_duration;
} AvSync;

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK,
};

#ifdef __cplusplus
extern "C" {
#endif

void av_sync_bind(AvSync *sync,
                  Clock *audclk,
                  Clock *vidclk,
                  Clock *extclk,
                  PacketQueue *audioq,
                  PacketQueue *videoq,
                  AVStream **audio_st,
                  int *audio_stream,
                  int *video_stream,
                  double *max_frame_duration);
int get_master_sync_type(const AvSync *sync);
double get_master_clock(const AvSync *sync);
void check_external_clock_speed(AvSync *sync);
double av_sync_compute_frame_delay(const AvSync *sync, Frame *lastvp, Frame *vp);
double compute_target_delay(double delay, const AvSync *sync);
double vp_duration(const AvSync *sync, Frame *vp, Frame *nextvp);
void update_video_pts(AvSync *sync, double pts, int serial);
void av_sync_update_video_pts_if_valid(AvSync *sync, double pts, int serial);
double av_sync_audio_master_diff(const AvSync *sync);
double av_sync_video_master_diff(const AvSync *sync, double video_clock);
int av_sync_is_external_clock_master(const AvSync *sync);
int av_sync_should_late_drop(const AvSync *sync, int step, double time, double frame_timer, double duration);
int av_sync_should_early_drop(const AvSync *sync,
                              double video_clock,
                              double frame_last_filter_delay,
                              int video_pkt_serial,
                              int video_clock_serial,
                              int video_queue_nb_packets);
void av_sync_sync_extclk_to_audclk(AvSync *sync);
void av_sync_seek_reset_extclk(AvSync *sync, int by_bytes, int64_t seek_target);
void av_sync_toggle_pause(AvSync *sync, int *paused, double *frame_timer, int read_pause_return);

#ifdef __cplusplus
}
#endif

#endif
