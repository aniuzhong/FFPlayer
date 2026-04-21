#include "av_sync.h"

#include <math.h>

#include "clock.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/time.h"

void av_sync_bind(AvSync *sync,
                  Clock *audclk,
                  Clock *vidclk,
                  Clock *extclk,
                  PacketQueue *audioq,
                  PacketQueue *videoq,
                  AVStream **audio_st,
                  int *audio_stream,
                  int *video_stream,
                  double *max_frame_duration)
{
    sync->audclk = audclk;
    sync->vidclk = vidclk;
    sync->extclk = extclk;
    sync->audioq = audioq;
    sync->videoq = videoq;
    sync->audio_st = audio_st;
    sync->audio_stream = audio_stream;
    sync->video_stream = video_stream;
    sync->max_frame_duration = max_frame_duration;
}

int get_master_sync_type(const AvSync *sync)
{
    if (sync->audio_st && *sync->audio_st)
        return AV_SYNC_AUDIO_MASTER;
    return AV_SYNC_EXTERNAL_CLOCK;
}

double get_master_clock(const AvSync *sync)
{
    double val;

    switch (get_master_sync_type(sync)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(sync->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(sync->audclk);
            break;
        default:
            val = get_clock(sync->extclk);
            break;
    }
    return val;
}

void check_external_clock_speed(AvSync *sync)
{
    if (*sync->video_stream >= 0 && packet_queue_get_nb_packets(sync->videoq) <= EXTERNAL_CLOCK_MIN_FRAMES ||
        *sync->audio_stream >= 0 && packet_queue_get_nb_packets(sync->audioq) <= EXTERNAL_CLOCK_MIN_FRAMES) {
        set_clock_speed(sync->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, clock_get_speed(sync->extclk) - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((*sync->video_stream < 0 || packet_queue_get_nb_packets(sync->videoq) > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (*sync->audio_stream < 0 || packet_queue_get_nb_packets(sync->audioq) > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(sync->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, clock_get_speed(sync->extclk) + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = clock_get_speed(sync->extclk);
        if (speed != 1.0)
            set_clock_speed(sync->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
    }
}

double compute_target_delay(double delay, const AvSync *sync)
{
    double sync_threshold, diff = 0;

    if (get_master_sync_type(sync) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(sync->vidclk) - get_master_clock(sync);

        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < *sync->max_frame_duration) {
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

double vp_duration(const AvSync *sync, Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > *sync->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

void update_video_pts(AvSync *sync, double pts, int serial)
{
    set_clock(sync->vidclk, pts, serial);
    sync_clock_to_slave(sync->extclk, sync->vidclk);
}

double av_sync_audio_master_diff(const AvSync *sync)
{
    return get_clock(sync->audclk) - get_master_clock(sync);
}

double av_sync_video_master_diff(const AvSync *sync, double video_clock)
{
    return video_clock - get_master_clock(sync);
}

void av_sync_toggle_pause(AvSync *sync, int *paused, double *frame_timer, int read_pause_return)
{
    if (*paused) {
        *frame_timer += av_gettime_relative() / 1000000.0 - clock_get_last_updated(sync->vidclk);
        if (read_pause_return != AVERROR(ENOSYS))
            clock_set_paused(sync->vidclk, 0);
        set_clock(sync->vidclk, get_clock(sync->vidclk), clock_get_serial(sync->vidclk));
    }
    set_clock(sync->extclk, get_clock(sync->extclk), clock_get_serial(sync->extclk));
    *paused = !*paused;
    clock_set_paused(sync->audclk, *paused);
    clock_set_paused(sync->vidclk, *paused);
    clock_set_paused(sync->extclk, *paused);
}
