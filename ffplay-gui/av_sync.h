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

#include "video_state.h"

enum {
    AV_SYNC_AUDIO_MASTER,
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK,
};

#ifdef __cplusplus
extern "C" {
#endif

int get_master_sync_type(VideoState *is);
double get_master_clock(VideoState *is);
void check_external_clock_speed(VideoState *is);
double compute_target_delay(double delay, VideoState *is);
double vp_duration(VideoState *is, Frame *vp, Frame *nextvp);
void update_video_pts(VideoState *is, double pts, int serial);

#ifdef __cplusplus
}
#endif

#endif
