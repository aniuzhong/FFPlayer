#include <libavutil/error.h>
#include <libavutil/log.h>

#include "demuxer.h"
#include "video_state.h"
#include "packet_queue.h"
#include "read_thread.h"
#include "stream.h"

static const char *demuxer_media_name(const Demuxer *d)
{
    return (d && d->input_url) ? d->input_url : "";
}

static int open_stream_components(VideoState *is)
{
    Demuxer *d = &is->demuxer;
    int vidx = demuxer_stream_index(d, AVMEDIA_TYPE_VIDEO);
    int aidx = demuxer_stream_index(d, AVMEDIA_TYPE_AUDIO);
    int sidx = demuxer_stream_index(d, AVMEDIA_TYPE_SUBTITLE);
    int ret;

    is->show_mode = SHOW_MODE_NONE;
    if (vidx >= 0) {
        AVRational sar = demuxer_guess_sample_aspect_ratio(d, vidx, NULL);
        int width  = demuxer_get_stream_width(d, vidx);
        int height = demuxer_get_stream_height(d, vidx);
        if (width && height && is->on_frame_size_changed)
            is->on_frame_size_changed(is->frame_size_opaque, width, height, sar);
    }

    if (aidx >= 0)
        stream_component_open(is, aidx);

    ret = -1;
    if (vidx >= 0)
        ret = stream_component_open(is, vidx);
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (sidx >= 0)
        stream_component_open(is, sidx);

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               demuxer_media_name(d));
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
            int r = demuxer_remote_pause(&is->demuxer);
            is->demuxer.read_pause_return = r;
        }
        else {
            demuxer_remote_play(&is->demuxer);
        }
    }
}

static void handle_seek_request(VideoState *is)
{
    Demuxer *demuxer = &is->demuxer;

    if (!is->seek_req)
        return;

    int64_t seek_target = is->seek_pos;
    int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
    int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

    int ret = demuxer_seek_file(demuxer, -1, seek_min, seek_target, seek_max, is->seek_flags);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: error while seeking\n",
               demuxer_media_name(demuxer));
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
    demuxer->queue_attachments_req = 1;
    demuxer->eof = 0;
    if (is->paused && is->on_step_frame)
        is->on_step_frame(is);
}

/* queue attached picture packets (e.g., album art) */
static int handle_queue_attachments(VideoState *is, AVPacket *pkt)
{
    if (!is->demuxer.queue_attachments_req)
        return 0;

    if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        int ret = av_packet_ref(pkt, &is->video_st->attached_pic);
        if (ret < 0)
            return ret;
        packet_queue_put(is->videoq, pkt);
        packet_queue_put_nullpacket(is->videoq, pkt, is->video_stream);
    }
    is->demuxer.queue_attachments_req = 0;
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

    is->demuxer.eof = 0;
    err = demuxer_open_input(&is->demuxer, NULL);
    if (err < 0) {
        ret = err;
        goto fail;
    }
    err = demuxer_find_stream_info(&is->demuxer, NULL);
    if (err < 0) {
        ret = err;
        goto fail;
    }
    demuxer_io_reset_eof(&is->demuxer);

    if (is->demuxer.seek_mode < 0) {
        int mode = demuxer_should_use_byte_seek(&is->demuxer);
        is->demuxer.seek_mode = mode;
    }

    is->demuxer.max_frame_duration = demuxer_get_max_gap(&is->demuxer);

    err = demuxer_find_stream_components(&is->demuxer);
    if (err < 0) {
        ret = err;
        goto fail;
    }

    if (open_stream_components(is) < 0) {
        ret = -1;
        goto fail;
    }

    if (is->infinite_buffer < 0 && demuxer_is_realtime(&is->demuxer))
        is->infinite_buffer = 1;

    for (;;) {
        if (is->demuxer.abort_request)
            break;

        handle_pause_resume(is);

        if (is->paused && demuxer_is_realtime_network_protocol(&is->demuxer)) {
            SDL_Delay(10);
            continue;
        }

        handle_seek_request(is);

        if (handle_queue_attachments(is, pkt) < 0) {
            ret = -1;
            goto fail;
        }

        /* throttle reading if queue is full or has enough packets (ffplay infinite_buffer) */
        int aq_size = packet_queue_get_size(is->audioq);
        int vq_size = packet_queue_get_size(is->videoq);
        int sq_size = packet_queue_get_size(is->subtitleq);
        if (is->infinite_buffer < 1 && (aq_size + vq_size + sq_size > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, is->subtitleq)))) {
            demuxer_wait_for_continue_reading(&is->demuxer, 10);
            continue;
        }

        ret = demuxer_read_packet(&is->demuxer, pkt);
        if (ret < 0) {
            if (demuxer_should_handle_eof(&is->demuxer, ret)) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(is->subtitleq, pkt, is->subtitle_stream);
                is->demuxer.eof = 1;
            }
            if (demuxer_is_io_error(&is->demuxer))
                break;
            demuxer_wait_for_continue_reading(&is->demuxer, 10);
            continue;
        } else {
            is->demuxer.eof = 0;
        }

        demuxer_handle_pkt_stream_events(&is->demuxer, pkt);
        route_packet_to_queue(is, pkt);
    }

    ret = 0;
fail:
    av_packet_free(&pkt);

    if (ret != 0)
        is->quit_request = 1;
    return 0;
}
