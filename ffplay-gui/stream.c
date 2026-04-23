#include <math.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/time.h>
#include <libavfilter/buffersink.h>

#include "packet_queue.h"
#include "clock.h"
#include "av_sync.h"
#include "demuxer.h"
#include "read_thread.h"
#include "filter.h"
#include "video_renderer.h"
#include "audio_thread.h"
#include "video_thread.h"
#include "subtitle_thread.h"
#include "stream.h"

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9

double stream_get_master_clock(VideoState *is)
{
    if (!is)
        return NAN;
    return get_master_clock(&is->av_sync);
}

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(demuxer_get_continue_read_thread(is->demuxer));
    }
}

void stream_toggle_pause(VideoState *is)
{
    av_sync_toggle_pause(&is->av_sync, &is->paused, &is->frame_timer, demuxer_get_read_pause_return(is->demuxer));
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
    if (!is || !is->audio_pipeline)
        return;
    is->audio_pipeline->muted = !is->audio_pipeline->muted;
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

void stream_set_volume(VideoState *is, int volume)
{
    if (!is || !is->audio_pipeline)
        return;
    is->audio_pipeline->audio_volume = av_clip(volume, 0, SDL_MIX_MAXVOLUME);
}

void stream_request_refresh(VideoState *is)
{
    if (!is)
        return;
    is->force_refresh = 1;
}

void stream_handle_window_size_changed(VideoState *is, int width, int height)
{
    if (!is)
        return;
    is->width = width;
    is->height = height;
    if (is->video_renderer && is->video_renderer->vis_texture) {
        SDL_DestroyTexture(is->video_renderer->vis_texture);
        is->video_renderer->vis_texture = NULL;
    }
    stream_request_refresh(is);
}

void stream_adjust_volume_step(VideoState *is, int sign, double step)
{
    double volume_level;
    int new_volume;

    if (!is)
        return;

    if (!is->audio_pipeline)
        return;

    volume_level = is->audio_pipeline->audio_volume ?
        (20 * log(is->audio_pipeline->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) :
        -1000.0;
    new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    stream_set_volume(is,
                      is->audio_pipeline->audio_volume == new_volume ? (is->audio_pipeline->audio_volume + sign) : new_volume);
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
    AVFormatContext *ic;

    if (!is)
        return;
    ic = demuxer_get_ic(is->demuxer);
    if (!ic || !ic->nb_chapters)
        return;

    pos = stream_get_master_clock(is) * AV_TIME_BASE;

    for (i = 0; i < ic->nb_chapters; i++) {
        AVChapter *ch = ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(ic->chapters[i]->start,
                                 ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

void stream_seek_relative(VideoState *is, double incr_seconds)
{
    double pos;
    AVFormatContext *ic;

    if (!is)
        return;
    ic = demuxer_get_ic(is->demuxer);
    if (!ic)
        return;

    if (demuxer_get_seek_mode(is->demuxer)) {
        pos = -1;
        if (pos < 0 && is->video_stream >= 0)
            pos = frame_queue_last_pos(is->pictq);
        if (pos < 0 && is->audio_stream >= 0)
            pos = frame_queue_last_pos(is->sampq);
        if (pos < 0)
            pos = avio_tell(ic->pb);
        if (ic->bit_rate)
            incr_seconds *= ic->bit_rate / 8.0;
        else
            incr_seconds *= 180000.0;
        pos += incr_seconds;
        stream_seek(is, (int64_t)pos, (int64_t)incr_seconds, 1);
    } else {
        pos = stream_get_master_clock(is);
        if (isnan(pos))
            pos = (double)is->seek_pos / AV_TIME_BASE;
        pos += incr_seconds;
        if (ic->start_time != AV_NOPTS_VALUE &&
            pos < ic->start_time / (double)AV_TIME_BASE)
            pos = ic->start_time / (double)AV_TIME_BASE;
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
    is->demuxer = demuxer_create(filename);
    if (!is->demuxer)
        goto fail;
    is->ytop    = 0;
    is->xleft   = 0;
    is->audio_device = audio_device;
    is->video_renderer = video_renderer;
    is->on_frame_size_changed = frame_size_changed_cb ? frame_size_changed_cb : stream_on_frame_size_changed;
    is->on_step_frame = stream_step;

    is->videoq = packet_queue_create();
    is->audioq = packet_queue_create();
    is->subtitleq = packet_queue_create();
    if (!is->videoq || !is->audioq || !is->subtitleq)
        goto fail;

    is->pictq = frame_queue_create(is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1);
    is->subpq = frame_queue_create(is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0);
    is->sampq = frame_queue_create(is->audioq, SAMPLE_QUEUE_SIZE, 1);
    if (!is->pictq || !is->subpq || !is->sampq)
        goto fail;

    is->vidclk = clock_create();
    is->audclk = clock_create();
    is->extclk = clock_create();
    if (!is->vidclk || !is->audclk || !is->extclk)
        goto fail;
    clock_init_from_packet_queue(is->vidclk, is->videoq);
    clock_init_from_packet_queue(is->audclk, is->audioq);
    clock_init_from_clock(is->extclk, is->extclk);
    av_sync_bind(&is->av_sync,
                 is->audclk,
                 is->vidclk,
                 is->extclk,
                 is->audioq,
                 is->videoq,
                 &is->audio_st,
                 &is->audio_stream,
                 &is->video_stream,
                 demuxer_get_max_frame_duration_ptr(is->demuxer));
    is->audio_pipeline = audio_pipeline_create();
    if (!is->audio_pipeline)
        goto fail;
    audio_pipeline_bind(is->audio_pipeline, &is->av_sync, is->sampq,
                        is->audioq, is->audio_device,
                        &is->paused, (int *)&is->show_mode);
    is->audio_pipeline->audio_clock_serial = -1;
    is->audio_pipeline->audio_volume = SDL_MIX_MAXVOLUME;
    is->audio_pipeline->muted = 0;
    if (demuxer_start(is->demuxer, read_thread, is) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to start demuxer thread\n");
        goto fail;
    }
    return is;

fail:
    stream_close(is);
    return NULL;
}

int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue)
{
    int64_t duration = packet_queue_get_duration(queue);

    return stream_id < 0 ||
           packet_queue_is_aborted(queue) ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           packet_queue_get_nb_packets(queue) > MIN_FRAMES &&
           (!duration || av_q2d(st->time_base) * duration > 1.0);
}



int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic;
    AVCodecContext *avctx;
    const AVCodec *codec;
    AVDictionary *opts = NULL;
    int sample_rate;
    AVChannelLayout ch_layout = { 0 };
    int ret = 0;
    
    ic = demuxer_get_ic(is->demuxer);
    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
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
    demuxer_set_eof(is->demuxer, 0);
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
        if ((ret = audio_device_open(is->audio_device, is->audio_pipeline, &ch_layout, sample_rate, &is->audio_pipeline->audio_tgt)) < 0)
            goto fail;
        is->audio_pipeline->audio_hw_buf_size = ret;
        is->audio_pipeline->audio_src = is->audio_pipeline->audio_tgt;
        is->audio_pipeline->audio_buf_size = 0;
        is->audio_pipeline->audio_buf_index = 0;
        audio_pipeline_init_sync(is->audio_pipeline);

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->auddec, avctx, is->audioq, demuxer_get_continue_read_thread(is->demuxer))) < 0)
            goto fail;
        if (ic->iformat->flags & AVFMT_NOTIMESTAMPS) {
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

        if ((ret = decoder_init(&is->viddec, avctx, is->videoq, demuxer_get_continue_read_thread(is->demuxer))) < 0)
            goto fail;
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
            goto out;
        demuxer_set_queue_attachments_req(is->demuxer, 1);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        if ((ret = decoder_init(&is->subdec, avctx, is->subtitleq, demuxer_get_continue_read_thread(is->demuxer))) < 0)
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
    AVFormatContext *ic;
    AVCodecParameters *codecpar;

    ic = demuxer_get_ic(is->demuxer);
    if (!ic || stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, is->sampq);
        if (is->audio_device)
            audio_device_close(is->audio_device);
        decoder_destroy(&is->auddec);
        audio_pipeline_reset(is->audio_pipeline);

        if (is->rdft) {
            av_tx_uninit(&is->rdft);
            av_freep(&is->real_data);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, is->subpq);
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
    demuxer_request_abort(is->demuxer);
    demuxer_stop(is->demuxer);
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    demuxer_free(&is->demuxer);
    if (packet_queue_is_initialized(is->videoq))
        packet_queue_free(&is->videoq);
    if (packet_queue_is_initialized(is->audioq))
        packet_queue_free(&is->audioq);
    if (packet_queue_is_initialized(is->subtitleq))
        packet_queue_free(&is->subtitleq);
    if (frame_queue_is_initialized(is->pictq))
        frame_queue_free(&is->pictq);
    if (frame_queue_is_initialized(is->sampq))
        frame_queue_free(&is->sampq);
    if (frame_queue_is_initialized(is->subpq))
        frame_queue_free(&is->subpq);
    sws_freeContext(is->video_renderer->sub_convert_ctx);
    is->video_renderer->sub_convert_ctx = NULL;
    if (is->video_renderer->vis_texture)
        SDL_DestroyTexture(is->video_renderer->vis_texture);
    if (is->video_renderer->vid_texture)
        SDL_DestroyTexture(is->video_renderer->vid_texture);
    if (is->video_renderer->sub_texture)
        SDL_DestroyTexture(is->video_renderer->sub_texture);
    audio_pipeline_free(&is->audio_pipeline);
    clock_destroy(&is->audclk);
    clock_destroy(&is->vidclk);
    clock_destroy(&is->extclk);
    av_free(is);
}

void stream_cycle_channel(VideoState *is, int codec_type)
{
    AVFormatContext *ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams;

    ic = demuxer_get_ic(is->demuxer);
    if (!ic)
        return;
    nb_streams = ic->nb_streams;

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
        st = ic->streams[p ? p->stream_index[stream_index] : stream_index];
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
