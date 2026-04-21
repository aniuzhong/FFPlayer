#include "av_sync.h"

#include <math.h>

#include "clock.h"
#include "libavutil/common.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"

int get_master_sync_type(VideoState *is)
{
    if (is->audio_st)
        return AV_SYNC_AUDIO_MASTER;
    return AV_SYNC_EXTERNAL_CLOCK;
}

double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

void check_external_clock_speed(VideoState *is)
{
    if (is->video_stream >= 0 && packet_queue_get_nb_packets(is->videoq) <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && packet_queue_get_nb_packets(is->audioq) <= EXTERNAL_CLOCK_MIN_FRAMES) {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || packet_queue_get_nb_packets(is->videoq) > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (is->audio_stream < 0 || packet_queue_get_nb_packets(is->audioq) > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}

double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);

    return delay;
}

double vp_duration(VideoState *is, Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

void update_video_pts(VideoState *is, double pts, int serial)
{
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}
