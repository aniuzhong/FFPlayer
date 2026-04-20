#ifndef FFPLAY_GUI_CLOCK_H
#define FFPLAY_GUI_CLOCK_H

#include "video_state.h"

#ifdef __cplusplus
extern "C" {
#endif

double get_clock(Clock *c);
void set_clock_at(Clock *c, double pts, int serial, double time);
void set_clock(Clock *c, double pts, int serial);
void set_clock_speed(Clock *c, double speed);
void init_clock(Clock *c, int *queue_serial);
void sync_clock_to_slave(Clock *c, Clock *slave);
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
