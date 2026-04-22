#include "demuxer.h"

#include <errno.h>
#include <string.h>

#include "stream.h"
#include "clock.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

static void print_error(const char *filename, int err)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, errbuf, sizeof(errbuf));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf);
}

static int decode_interrupt_cb(void *ctx)
{
    Demuxer *demuxer = (Demuxer *)ctx;
    return demuxer_is_aborted(demuxer);
}

int demuxer_init(Demuxer *demuxer, const char *input_url)
{
    if (!demuxer || !input_url)
        return AVERROR(EINVAL);
    memset(demuxer, 0, sizeof(*demuxer));
    demuxer->seek_mode = -1;
    demuxer->input_url = av_strdup(input_url);
    if (!demuxer->input_url)
        return AVERROR(ENOMEM);
    return 0;
}

void demuxer_destroy(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    avformat_close_input(&demuxer->ic);
    av_freep(&demuxer->input_url);
    demuxer->seek_mode = -1;
    demuxer->abort_request = 0;
}

void demuxer_request_abort(Demuxer *demuxer)
{
    if (!demuxer)
        return;
    demuxer->abort_request = 1;
}

int demuxer_is_aborted(const Demuxer *demuxer)
{
    if (!demuxer)
        return 0;
    return demuxer->abort_request;
}

int demuxer_get_seek_mode(const Demuxer *demuxer)
{
    if (!demuxer || demuxer->seek_mode < 0)
        return 0;
    return demuxer->seek_mode;
}

const char *demuxer_get_input_name(const Demuxer *demuxer)
{
    if (!demuxer || !demuxer->input_url)
        return "";
    return demuxer->input_url;
}

/* this thread gets the stream from the disk or the network */
int read_thread(void *arg)
{
    VideoState *is = (VideoState *)arg;
    Demuxer *demuxer = &is->demuxer;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket *pkt = NULL;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    AVDictionary *open_opts = NULL;

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    pkt = av_packet_alloc();
    if (!pkt) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = demuxer;
    if (!av_dict_get(open_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE))
        av_dict_set(&open_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
    err = avformat_open_input(&ic, demuxer->input_url, NULL, &open_opts);
    av_dict_free(&open_opts);
    open_opts = NULL;
    if (err < 0) {
        print_error(demuxer->input_url, err);
        ret = -1;
        goto fail;
    }
    demuxer->ic = ic;

    err = avformat_find_stream_info(ic, NULL);

    if (err < 0) {
        av_log(NULL, AV_LOG_WARNING,
               "%s: could not find codec parameters\n", demuxer->input_url);
        ret = -1;
        goto fail;
    }

    if (ic->pb)
        ic->pb->eof_reached = 0;

    if (demuxer->seek_mode < 0)
        demuxer->seek_mode = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                             !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                             strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

    is->realtime = is_realtime(ic);

    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        st->discard = AVDISCARD_ALL;
        st->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }

    st_index[AVMEDIA_TYPE_VIDEO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                            st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    st_index[AVMEDIA_TYPE_AUDIO] =
        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                            st_index[AVMEDIA_TYPE_AUDIO],
                            st_index[AVMEDIA_TYPE_VIDEO],
                            NULL, 0);
    st_index[AVMEDIA_TYPE_SUBTITLE] =
        av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                            st_index[AVMEDIA_TYPE_SUBTITLE],
                            (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                             st_index[AVMEDIA_TYPE_AUDIO] :
                             st_index[AVMEDIA_TYPE_VIDEO]),
                            NULL, 0);

    is->show_mode = SHOW_MODE_NONE;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width && is->on_frame_size_changed)
            is->on_frame_size_changed(is, codecpar->width, codecpar->height, sar);
    }

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               demuxer->input_url);
        ret = -1;
        goto fail;
    }

    for (;;) {
        if (demuxer_is_aborted(demuxer))
            break;
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && demuxer->input_url && !strncmp(demuxer->input_url, "mmsh:", 5)))) {
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

            ret = avformat_seek_file(demuxer->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", demuxer->ic->url);
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
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused && is->on_step_frame)
                is->on_step_frame(is);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(is->videoq, pkt);
                packet_queue_put_nullpacket(is->videoq, pkt, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        if (!is->realtime &&
              (packet_queue_get_size(is->audioq) +
               packet_queue_get_size(is->videoq) +
               packet_queue_get_size(is->subtitleq) > MAX_QUEUE_SIZE
            || (stream_has_enough_packets(is->audio_st, is->audio_stream, is->audioq) &&
                stream_has_enough_packets(is->video_st, is->video_stream, is->videoq) &&
                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, is->subtitleq)))) {
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(is->videoq, pkt, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(is->audioq, pkt, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(is->subtitleq, pkt, is->subtitle_stream);
                is->eof = 1;
            }
            if (ic->pb && ic->pb->error)
                break;
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        } else {
            is->eof = 0;
        }

        ic->streams[pkt->stream_index]->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;

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

    ret = 0;
fail:
    if (ic && !demuxer->ic)
        avformat_close_input(&ic);

    av_packet_free(&pkt);
    if (ret != 0) {
        SDL_Event event;
        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}
