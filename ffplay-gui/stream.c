#include "stream.h"

#include <math.h>
#include <string.h>

#include "clock.h"
#include "demuxer.h"
#include "filter.h"
#include "video_renderer.h"
#include "audio_thread.h"
#include "video_thread.h"
#include "subtitle_thread.h"
#include "libavutil/error.h"
#include "libavutil/time.h"

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

void stream_toggle_pause(VideoState *is)
{
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
}

void stream_toggle_pause_and_clear_step(VideoState *is)
{
    if (!is)
        return;
    stream_toggle_pause(is);
    is->step = 0;
}

void stream_toggle_mute(VideoState *is)
{
    if (!is)
        return;
    is->muted = !is->muted;
}

void stream_toggle_audio_display(VideoState *is)
{
    int next;

    if (!is)
        return;

    next = is->show_mode;
    do {
        next = (next + 1) % SHOW_MODE_NB;
    } while (next != is->show_mode &&
             (next == SHOW_MODE_VIDEO && !is->video_st ||
              next != SHOW_MODE_VIDEO && !is->audio_st));

    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = (enum ShowMode)next;
    }
}

void stream_adjust_volume_step(VideoState *is, int sign, double step)
{
    double volume_level;
    int new_volume;

    if (!is)
        return;

    volume_level = is->audio_volume ?
        (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) :
        -1000.0;
    new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(
        is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume,
        0, SDL_MIX_MAXVOLUME);
}

void stream_step(VideoState *is)
{
    if (!is)
        return;
    /* If paused, unpause first and then arm single-step mode. */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

void stream_seek_chapter(VideoState *is, int incr)
{
    int64_t pos;
    int i;

    if (!is || !is->ic || !is->ic->nb_chapters)
        return;

    pos = get_master_clock(is) * AV_TIME_BASE;

    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start,
                                 is->ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

void stream_seek_relative(VideoState *is, double incr_seconds)
{
    double pos;

    if (!is || !is->ic)
        return;

    if (demuxer_get_seek_mode(is->demuxer)) {
        pos = -1;
        if (pos < 0 && is->video_stream >= 0)
            pos = frame_queue_last_pos(&is->pictq);
        if (pos < 0 && is->audio_stream >= 0)
            pos = frame_queue_last_pos(&is->sampq);
        if (pos < 0)
            pos = avio_tell(is->ic->pb);
        if (is->ic->bit_rate)
            incr_seconds *= is->ic->bit_rate / 8.0;
        else
            incr_seconds *= 180000.0;
        pos += incr_seconds;
        stream_seek(is, (int64_t)pos, (int64_t)incr_seconds, 1);
    } else {
        pos = get_master_clock(is);
        if (isnan(pos))
            pos = (double)is->seek_pos / AV_TIME_BASE;
        pos += incr_seconds;
        if (is->ic->start_time != AV_NOPTS_VALUE &&
            pos < is->ic->start_time / (double)AV_TIME_BASE)
            pos = is->ic->start_time / (double)AV_TIME_BASE;
        stream_seek(is,
                    (int64_t)(pos * AV_TIME_BASE),
                    (int64_t)(incr_seconds * AV_TIME_BASE),
                    0);
    }
}

static void stream_on_frame_size_changed(VideoState *is, int width, int height, AVRational sar)
{
    if (!is || !is->video_renderer)
        return;
    video_renderer_set_default_window_size(is->video_renderer, is, width, height, sar);
}

VideoState *stream_open(const char *filename,
                        AudioDevice *audio_device,
                        VideoRenderer *video_renderer,
                        void (*frame_size_changed_cb)(VideoState *is, int width, int height, AVRational sar))
{
    VideoState *is;

    if (!filename || !audio_device || !video_renderer)
        return NULL;

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->ytop    = 0;
    is->xleft   = 0;
    is->audio_device = audio_device;
    is->video_renderer = video_renderer;
    is->on_frame_size_changed = frame_size_changed_cb ? frame_size_changed_cb : stream_on_frame_size_changed;
    is->demuxer = demuxer_create(is, filename, stream_step);
    if (!is->demuxer)
        goto fail;

    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    is->audio_volume = SDL_MIX_MAXVOLUME;
    is->muted = 0;
    is->read_tid = SDL_CreateThread(read_thread, "read_thread", is->demuxer);
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        goto fail;
    }
    return is;

fail:
    stream_close(is);
    return NULL;
}

int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
}

int is_realtime(AVFormatContext *s)
{
    if (!strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp")) {
        return 1;
    }

    if (s->pb && (!strncmp(s->url, "rtp:", 4)
        || !strncmp(s->url, "udp:", 4))) {
        return 1;
    }
    return 0;
}

int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    AVDictionary *opts = NULL;
    int sample_rate;
    AVChannelLayout ch_layout = { 0 };
    int ret = 0;
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:    is->last_audio_stream = stream_index; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; break;
        case AVMEDIA_TYPE_VIDEO:    is->last_video_stream = stream_index; break;
    }
    if (!codec) {
        av_log(NULL, AV_LOG_WARNING,
               "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    avctx->lowres = 0;

    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);

    av_dict_set(&opts, "flags", "+copy_opaque", AV_DICT_MULTIKEY);

    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq = avctx->sample_rate;
            ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
            if (ret < 0)
                goto fail;
            is->audio_filter_src.fmt = avctx->sample_fmt;
            if ((ret = configure_audio_filters(is, NULL, 0)) < 0)
                goto fail;
            sink = is->out_audio_filter;
            sample_rate = av_buffersink_get_sample_rate(sink);
            ret = av_buffersink_get_ch_layout(sink, &ch_layout);
            if (ret < 0)
                goto fail;
        }

        if (!is->audio_device) {
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if ((ret = audio_device_open(is->audio_device, is, &ch_layout, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size = 0;
        is->audio_buf_index = 0;

        is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0)
            goto fail;
        if (is->ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0)
            goto out;
        audio_device_pause(is->audio_device, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
            goto out;
        is->queue_attachments_req = 1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0)
            goto fail;
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_channel_layout_uninit(&ch_layout);
    av_dict_free(&opts);
    return ret;
}

void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        if (is->audio_device)
            audio_device_close(is->audio_device);
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_tx_uninit(&is->rdft);
            av_freep(&is->real_data);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

void stream_close(VideoState *is)
{
    if (!is)
        return;
    is->abort_request = 1;
    if (is->read_tid) {
        SDL_WaitThread(is->read_tid, NULL);
        is->read_tid = NULL;
    }
    demuxer_destroy(&is->demuxer);

    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);
    if (is->videoq.mutex && is->videoq.cond && is->videoq.pkt_list)
        packet_queue_destroy(&is->videoq);
    if (is->audioq.mutex && is->audioq.cond && is->audioq.pkt_list)
        packet_queue_destroy(&is->audioq);
    if (is->subtitleq.mutex && is->subtitleq.cond && is->subtitleq.pkt_list)
        packet_queue_destroy(&is->subtitleq);
    if (is->pictq.mutex && is->pictq.cond)
        frame_queue_destroy(&is->pictq);
    if (is->sampq.mutex && is->sampq.cond)
        frame_queue_destroy(&is->sampq);
    if (is->subpq.mutex && is->subpq.cond)
        frame_queue_destroy(&is->subpq);
    if (is->continue_read_thread)
        SDL_DestroyCond(is->continue_read_thread);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    if (is->vis_texture)
        SDL_DestroyTexture(is->vis_texture);
    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);
    av_free(is);
}

void stream_cycle_channel(VideoState *is, int codec_type)
{
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams) {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                    st->codecpar->ch_layout.nb_channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string((enum AVMediaType)codec_type),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}
