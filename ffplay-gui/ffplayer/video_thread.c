#include <math.h>

#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include "av_sync.h"
#include "clock.h"
#include "demuxer.h"
#include "filter.h"
#include "stream.h"
#include "video_state.h"
#include "video_thread.h"
#include "packet_queue.h"

/* When hardware decoding silently produces zero frames (e.g. the
 * D3D11VA decoder accepted our hwframes pool but the actual codec
 * profile is unsupported on this GPU) we want to recover gracefully
 * rather than show a black screen forever. We watch wall-clock time
 * since the last successfully-queued frame and only act after the
 * threshold expires AND there is real decoder work in flight (the
 * packet queue is being filled). */
#define HW_DECODE_STALL_THRESHOLD_US (3LL * 1000 * 1000)
#define HW_DECODE_STALL_MIN_QUEUE     8

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    Frame *vp;

    if (!(vp = frame_queue_peek_writable(is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    if (is->on_frame_size_changed)
        is->on_frame_size_changed(is->frame_size_opaque, vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = demuxer_guess_sample_aspect_ratio(is->demuxer, 
                                                                       demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO),
                                                                       frame);

        if (frame->pts != AV_NOPTS_VALUE &&
            av_sync_should_early_drop(&is->av_sync,
                                      dpts,
                                      is->frame_last_filter_delay,
                                      is->viddec.pkt_serial,
                                      clock_get_serial(is->vidclk),
                                      packet_queue_get_nb_packets(is->videoq))) {
            is->frame_drops_early++;
            av_frame_unref(frame);
            got_picture = 0;
        }
    }

    return got_picture;
}

/* Hardware-decoded frames keep their pixel data on the GPU and have no
 * representation that the libavfilter graph could meaningfully consume
 * (filtering would require av_hwframe_transfer_data, which defeats the
 * whole zero-copy goal). For the HW path we therefore push the frame
 * straight into the picture queue, computing pts/duration directly from
 * the stream's time base and the demuxer-guessed frame rate. */
static int hw_frame_has_data(const AVFrame *frame)
{
    return frame && frame->hw_frames_ctx && frame->data[0];
}

int video_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    AVFrame *frame = av_frame_alloc();
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = demuxer_guess_frame_rate(is->demuxer, demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO), NULL);

    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = (enum AVPixelFormat)-2;
    int last_serial = -1;

    /* Watchdog state for HW->SW fallback. We only care about elapsed
     * un-paused wall-clock time; while paused we freeze the timer. */
    int64_t hw_last_progress_us = av_gettime_relative();
    int     hw_fallback_attempted = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;

        /* Reset the HW watchdog whenever we either produced a frame or
         * we are paused (no work is expected). Otherwise, if we have
         * been failing for a while AND there are clearly packets to
         * decode, hot-swap the codec to a SW context and continue. */
        if (ret > 0) {
            hw_last_progress_us = av_gettime_relative();
        } else if (!hw_fallback_attempted &&
                   is->viddec.avctx &&
                   is->viddec.avctx->hw_device_ctx &&
                   !stream_is_paused(is)) {
            int64_t now_us  = av_gettime_relative();
            int     q_count = packet_queue_get_nb_packets(is->videoq);
            if (q_count > HW_DECODE_STALL_MIN_QUEUE &&
                now_us - hw_last_progress_us > HW_DECODE_STALL_THRESHOLD_US) {
                av_log(NULL, AV_LOG_ERROR,
                       "Hardware video decode produced no frames for >%lldms "
                       "with %d packets queued; falling back to software.\n",
                       (long long)(HW_DECODE_STALL_THRESHOLD_US / 1000), q_count);
                hw_fallback_attempted = 1;
                if (stream_video_reopen_software(is) < 0) {
                    av_log(NULL, AV_LOG_ERROR,
                           "Software fallback failed; aborting playback.\n");
                    goto the_end;
                }
                /* Reset filter-graph state because the new SW context
                 * will produce frames in a different pixel format than
                 * the (silently broken) HW path was attempting to. */
                avfilter_graph_free(&graph);
                filt_in = filt_out = NULL;
                last_w = last_h = 0;
                last_format = (enum AVPixelFormat)-2;
                last_serial = -1;
                hw_last_progress_us = av_gettime_relative();
            }
        }

        if (!ret)
            continue;

        /* ---- HW (zero-copy) path: bypass libavfilter entirely. -- */
        if (hw_frame_has_data(frame)) {
            FrameData *fd = frame->opaque_ref ? (FrameData *)frame->opaque_ref->data : NULL;
            AVRational hw_tb = is->video_st->time_base;
            AVRational hw_fr = demuxer_guess_frame_rate(is->demuxer,
                demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO), frame);

            duration = (hw_fr.num && hw_fr.den ? av_q2d((AVRational){hw_fr.den, hw_fr.num}) : 0);
            pts      = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(hw_tb);

            /* SAR may be unset on hwframes; the get_video_frame() helper
             * already applied demuxer_guess_sample_aspect_ratio. */
            ret = queue_picture(is, frame, pts, duration, fd ? fd->pkt_pos : -1, is->viddec.pkt_serial);
            av_frame_unref(frame);
            if (ret < 0)
                goto the_end;
            continue;
        }

        if (last_w != frame->width
            || last_h != frame->height
            || last_format != (enum AVPixelFormat)frame->format
            || last_serial != is->viddec.pkt_serial) {
            const char *last_fmt_name = av_get_pix_fmt_name(last_format);
            const char *curr_fmt_name = av_get_pix_fmt_name((enum AVPixelFormat)frame->format);
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   last_fmt_name ? last_fmt_name : "none", last_serial,
                   frame->width, frame->height,
                   curr_fmt_name ? curr_fmt_name : "none", is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            if (!is->nb_supported_pix_fmts) {
                ret = AVERROR(EINVAL);
                goto the_end;
            }
            if ((ret = configure_video_filters(graph, is->demuxer, is->video_st, NULL,
                                               frame, is->supported_pix_fmts, is->nb_supported_pix_fmts,
                                               &is->in_video_filter, &is->out_video_filter)) < 0) {
                is->quit_request = 1;
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = (enum AVPixelFormat)frame->format;
            last_serial = is->viddec.pkt_serial;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            FrameData *fd;

            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            fd = frame->opaque_ref ? (FrameData *)frame->opaque_ref->data : NULL;

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (av_sync_should_clear_frame_filter_delay(is->frame_last_filter_delay))
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, fd ? fd->pkt_pos : -1, is->viddec.pkt_serial);
            av_frame_unref(frame);
            if (packet_queue_get_serial(is->videoq) != is->viddec.pkt_serial)
                break;
        }

        if (ret < 0)
            goto the_end;
    }
the_end:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    return 0;
}
