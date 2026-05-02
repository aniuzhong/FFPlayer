#include <math.h>

#include <libavutil/common.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>

#include "av_sync.h"
#include "clock.h"
#include "packet_queue.h"

static int av_sync_vid_serial_authority(const AvSync *sync)
{
    return packet_queue_get_serial(sync->videoq);
}

static int av_sync_aud_serial_authority(const AvSync *sync)
{
    return packet_queue_get_serial(sync->audioq);
}

static int av_sync_ext_serial_authority(const AvSync *sync)
{
    return clock_get_serial(sync->extclk);
}

/* Context binding */
void av_sync_bind(AvSync *sync, Clock *audclk, Clock *vidclk, Clock *extclk,
                  PacketQueue *audioq, PacketQueue *videoq, AVStream **audio_st,
                  int *audio_stream, int *video_stream, double *max_frame_duration)
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

/* Master-clock query */
int get_master_sync_type(const AvSync *sync)
{
    if (sync->audio_st && *sync->audio_st)
        return AV_SYNC_AUDIO_MASTER;
    return AV_SYNC_EXTERNAL_CLOCK;
}

int av_sync_is_audio_master(const AvSync *sync)
{
    return get_master_sync_type(sync) == AV_SYNC_AUDIO_MASTER;
}

double get_master_clock(const AvSync *sync)
{
    double val;

    switch (get_master_sync_type(sync)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(sync->vidclk, av_sync_vid_serial_authority(sync));
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(sync->audclk, av_sync_aud_serial_authority(sync));
            break;
        default:
            val = get_clock(sync->extclk, av_sync_ext_serial_authority(sync));
            break;
    }
    return val;
}

/* Sync control and delay calculation */
void check_external_clock_speed(AvSync *sync)
{
    if (*sync->video_stream >= 0 && packet_queue_get_nb_packets(sync->videoq) <= EXTERNAL_CLOCK_MIN_FRAMES ||
        *sync->audio_stream >= 0 && packet_queue_get_nb_packets(sync->audioq) <= EXTERNAL_CLOCK_MIN_FRAMES) {
        set_clock_speed(sync->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, clock_get_speed(sync->extclk) - EXTERNAL_CLOCK_SPEED_STEP),
                        av_sync_ext_serial_authority(sync));
    } else if ((*sync->video_stream < 0 || packet_queue_get_nb_packets(sync->videoq) > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (*sync->audio_stream < 0 || packet_queue_get_nb_packets(sync->audioq) > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(sync->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, clock_get_speed(sync->extclk) + EXTERNAL_CLOCK_SPEED_STEP),
                        av_sync_ext_serial_authority(sync));
    } else {
        double speed = clock_get_speed(sync->extclk);
        if (speed != 1.0)
            set_clock_speed(sync->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed),
                            av_sync_ext_serial_authority(sync));
    }
}

double av_sync_compute_frame_delay(const AvSync *sync, Frame *lastvp, Frame *vp)
{
    double last_duration = vp_duration(sync, lastvp, vp);
    return compute_target_delay(last_duration, sync);
}

/* Keep this low-level helper exposed for compatibility. */
double compute_target_delay(double delay, const AvSync *sync)
{
    double sync_threshold, diff = 0;

    if (get_master_sync_type(sync) != AV_SYNC_VIDEO_MASTER) {
        diff = get_clock(sync->vidclk, av_sync_vid_serial_authority(sync)) - get_master_clock(sync);

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

/* Clock update helpers */
void update_video_pts(AvSync *sync, double pts, int serial)
{
    set_clock(sync->vidclk, pts, serial);
    sync_clock_to_slave(sync->extclk, av_sync_ext_serial_authority(sync),
                        sync->vidclk, av_sync_vid_serial_authority(sync));
}

void av_sync_update_video_pts_if_valid(AvSync *sync, double pts, int serial)
{
    if (!isnan(pts))
        update_video_pts(sync, pts, serial);
}

double av_sync_audio_master_diff(const AvSync *sync)
{
    return get_clock(sync->audclk, av_sync_aud_serial_authority(sync)) - get_master_clock(sync);
}

double av_sync_video_master_diff(const AvSync *sync, double video_clock)
{
    return video_clock - get_master_clock(sync);
}

int av_sync_is_external_clock_master(const AvSync *sync)
{
    return get_master_sync_type(sync) == AV_SYNC_EXTERNAL_CLOCK;
}

int av_sync_should_late_drop(const AvSync *sync, int step, double time, double frame_timer, double duration)
{
    return !step &&
           get_master_sync_type(sync) != AV_SYNC_VIDEO_MASTER &&
           time > frame_timer + duration;
}

int av_sync_should_reset_frame_timer(double delay, double time, double frame_timer)
{
    return delay > 0 && time - frame_timer > AV_SYNC_THRESHOLD_MAX;
}

int av_sync_should_early_drop(const AvSync *sync,
                              double video_clock,
                              double frame_last_filter_delay,
                              int video_pkt_serial,
                              int video_clock_serial,
                              int video_queue_nb_packets)
{
    double diff;

    if (get_master_sync_type(sync) == AV_SYNC_VIDEO_MASTER)
        return 0;

    diff = av_sync_video_master_diff(sync, video_clock);
    return !isnan(diff) &&
           fabs(diff) < AV_NOSYNC_THRESHOLD &&
           diff - frame_last_filter_delay < 0 &&
           video_pkt_serial == video_clock_serial &&
           video_queue_nb_packets;
}

int av_sync_should_clear_frame_filter_delay(double frame_last_filter_delay)
{
    return fabs(frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0;
}

void av_sync_sync_extclk_to_audclk(AvSync *sync)
{
    sync_clock_to_slave(sync->extclk, av_sync_ext_serial_authority(sync),
                        sync->audclk, av_sync_aud_serial_authority(sync));
}

void av_sync_seek_reset_extclk(AvSync *sync, int by_bytes, int64_t seek_target)
{
    if (by_bytes)
        set_clock(sync->extclk, NAN, 0);
    else
        set_clock(sync->extclk, seek_target / (double)AV_TIME_BASE, 0);
}

void av_sync_update_audclk_from_callback(AvSync *sync,
                                         double audio_clock,
                                         int audio_clock_serial,
                                         int audio_hw_buf_size,
                                         int audio_write_buf_size,
                                         int audio_tgt_bytes_per_sec,
                                         int64_t audio_callback_time)
{
    set_clock_at(sync->audclk,
                 audio_clock - (double)(2 * audio_hw_buf_size + audio_write_buf_size) / audio_tgt_bytes_per_sec,
                 audio_clock_serial,
                 audio_callback_time / 1000000.0);
}

/* Pause/resume orchestration helper */
void av_sync_toggle_pause(AvSync *sync, int *paused, double *frame_timer, int read_pause_return)
{
    if (*paused) {
        *frame_timer += av_gettime_relative() / 1000000.0 - clock_get_last_updated(sync->vidclk);
        if (read_pause_return != AVERROR(ENOSYS))
            clock_set_paused(sync->vidclk, 0);
        set_clock(sync->vidclk,
                  get_clock(sync->vidclk, av_sync_vid_serial_authority(sync)),
                  clock_get_serial(sync->vidclk));
    }
    set_clock(sync->extclk,
              get_clock(sync->extclk, av_sync_ext_serial_authority(sync)),
              clock_get_serial(sync->extclk));
    *paused = !*paused;
    clock_set_paused(sync->audclk, *paused);
    clock_set_paused(sync->vidclk, *paused);
    clock_set_paused(sync->extclk, *paused);
}
