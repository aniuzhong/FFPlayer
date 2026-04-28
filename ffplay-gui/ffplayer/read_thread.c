#include <libavutil/error.h>
#include <libavutil/log.h>

#include "demuxer.h"
#include "video_state.h"
#include "packet_queue.h"
#include "read_thread.h"
#include "stream.h"

static int open_stream_components(VideoState *is)
{
    int ret = -1;

    is->show_mode = SHOW_MODE_NONE;
    if (demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO) >= 0) {
        AVRational sar = demuxer_guess_sample_aspect_ratio(is->demuxer, demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO), NULL);
        int width = demuxer_get_stream_width(is->demuxer, demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO));
        int height = demuxer_get_stream_height(is->demuxer, demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO));
        if (width && height && is->on_frame_size_changed)
            is->on_frame_size_changed(is->frame_size_opaque, width, height, sar);
    }

    if (demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_AUDIO) >= 0) {
        stream_component_open(is, demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_AUDIO));
    }

    ret = -1;
    if (demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO) >= 0) {
        ret = stream_component_open(is, demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_VIDEO));
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_SUBTITLE) >= 0) {
        stream_component_open(is, demuxer_get_stream_index(is->demuxer, AVMEDIA_TYPE_SUBTITLE));
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               demuxer_get_input_name(is->demuxer));
        return -1;
    }

    return 0;
}

/* handle pause and resume for the stream */
static void handle_pause_resume(VideoState *is)
{
    if (is->paused != is->last_paused) {
        is->last_paused = is->paused;
        if (is->paused) {
            int ret = demuxer_remote_pause(is->demuxer);
            demuxer_set_read_pause_return(is->demuxer, ret);
        }
        else {
            demuxer_remote_play(is->demuxer);
        }
    }
}

static int handle_seek_request(VideoState *is)
{
    Demuxer *demuxer = is->demuxer;

    if (!is->seek_req)
        return 0;
    
    int64_t seek_target = is->seek_pos;
    int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
    int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

    int ret = demuxer_seek_file(demuxer, -1, seek_min, seek_target, seek_max, is->seek_flags);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n", demuxer_get_input_name(demuxer));
    } else {
        if (is->audio_stream >= 0)
            packet_queue_flush(is->audioq);
        if (is->subtitle_stream >= 0)
            packet_queue_flush(is->subtitleq);
        if (is->video_stream >= 0)
            packet_queue_flush(is->videoq);
        av_sync_seek_reset_extclk(&is->av_sync, !!(is->seek_flags & AVSEEK_FLAG_BYTE), seek_target);
    }
    is->seek_req = 0;
    demuxer_set_queue_attachments_req(demuxer, 1);
    demuxer_set_eof(demuxer, 0);
    if (is->paused && is->on_step_frame)
        is->on_step_frame(is);
    
    return 0;
}

/* queue attached picture packets (e.g., album art) */
static int handle_queue_attachments(VideoState *is, AVPacket *pkt)
{
    if (!demuxer_get_queue_attachments_req(is->demuxer))
        return 0;
    
    if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        int ret = av_packet_ref(pkt, &is->video_st->attached_pic);
        if (ret < 0)
            return ret;
        packet_queue_put(is->videoq, pkt);
        packet_queue_put_nullpacket(is->videoq, pkt, is->video_stream);
    }
    demuxer_set_queue_attachments_req(is->demuxer, 0);
    return 0;
}

/* route packet to appropriate queue based on stream index */
static void route_packet_to_queue(VideoState *is, AVPacket *pkt)
{
    if (pkt->stream_index == is->audio_stream) {
        packet_queue_put(is->audioq, pkt);
    } else if (pkt->stream_index == is->video_stream
               && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        packet_queue_put(is->videoq, pkt);
    } else if (pkt->stream_index == is->subtitle_stream) {
        packet_queue_put(is->subtitleq, pkt);
    } else {
        av_packet_unref(pkt);
    }
}

/* this thread gets the stream from the disk or the network */
int read_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    int err, ret = 0;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    demuxer_set_eof(is->demuxer, 0);
    demuxer_open_input(is->demuxer, NULL);
    demuxer_find_stream_info(is->demuxer, NULL);
    demuxer_io_reset_eof(is->demuxer);

    // If not explicitly set a seek mode, auto-detect it based on format flags.
    if (demuxer_get_seek_mode(is->demuxer) < 0) {
        int mode = demuxer_should_use_byte_seek(is->demuxer);
        demuxer_set_seek_mode(is->demuxer, mode);
    }

    double max_duration = demuxer_get_max_gap(is->demuxer);
    demuxer_set_max_frame_duration(is->demuxer, max_duration);

    if (demuxer_find_stream_components(is->demuxer) < 0) {
        ret = -1;
        goto fail;
    }

    if (open_stream_components(is) < 0) {
        ret = -1;
        goto fail;
    }

    for (;;) {
        if (demuxer_is_aborted(is->demuxer))
            break;

        handle_pause_resume(is);

        if (is->paused && demuxer_is_realtime_network_protocol(is->demuxer)) {
            SDL_Delay(10);
            continue;
        }

        if (handle_seek_request(is) < 0) {
            ret = -1;
            goto fail;
        }
        
        if (handle_queue_attachments(is, pkt) < 0) {
            ret = -1;
            goto fail;
        }

        /* throttle reading if queue is full or has enough packets */
        if (!demuxer_is_realtime(is->demuxer) &&
              (packet_queue_get_size(is->audioq) +
               packet_queue_get_size(is->videoq) +
               packet_queue_get_size(is->subtitleq) > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, is->subtitleq)))) {
            demuxer_wait_for_continue_reading(is->demuxer, 10);
            continue;
        }
        
        ret = demuxer_read_packet(is->demuxer, pkt);
        if (ret < 0) {
            if (demuxer_should_handle_eof(is->demuxer, ret)) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(is->subtitleq, pkt, is->subtitle_stream);
                demuxer_set_eof(is->demuxer, 1);
            }
            if (demuxer_is_io_error(is->demuxer))
                break;
            demuxer_wait_for_continue_reading(is->demuxer, 10);
            continue;
        } else {
            demuxer_set_eof(is->demuxer, 0);
        }

        route_packet_to_queue(is, pkt);
    }

    /* The for-loop exits via break (abort / pb->error), not via goto fail.
       Only goto-fail paths set ret to a non-zero error code before jumping. */
    ret = 0;
fail:
    av_packet_free(&pkt);

    if (ret != 0)
        is->quit_request = 1;
    return 0;
}
