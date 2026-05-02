#include <math.h>
#include <string.h>

#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavformat/avio.h>
#include <libavfilter/buffersink.h>

#ifdef _WIN32
#include <d3d11.h>
#include <libavutil/hwcontext_d3d11va.h>
#endif

#include "audio_device.h"
#include "packet_queue.h"
#include "clock.h"
#include "av_sync.h"
#include "demuxer.h"
#include "read_thread.h"
#include "filter.h"
#include "audio_thread.h"
#include "video_thread.h"
#include "subtitle_thread.h"
#include "audio_visualizer.h"
#include "video_state.h"
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

/* AVCodecContext::get_format callback: when the decoder offers a
 * hardware-backed pixel format that matches our renderer-owned
 * AVHWDeviceContext (D3D11VA today), we accept it and pre-allocate the
 * frames pool ourselves. Pre-allocating lets us tag the surfaces with
 * D3D11_BIND_SHADER_RESOURCE so the renderer can bind them to a pixel
 * shader directly. If no hw format is offered we fall back to the
 * first sw format and the legacy filter path keeps working. */
static enum AVPixelFormat ffplay_get_format(AVCodecContext *ctx,
                                            const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;
    if (!ctx->hw_device_ctx)
        return pix_fmts[0];

    AVHWDeviceContext *dctx = (AVHWDeviceContext *)ctx->hw_device_ctx->data;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        int matches = 0;
#ifdef AV_HWDEVICE_TYPE_D3D11VA
        if (*p == AV_PIX_FMT_D3D11 && dctx->type == AV_HWDEVICE_TYPE_D3D11VA)
            matches = 1;
#endif
        if (!matches)
            continue;

        /* Let the codec pick width/height/sw_format that match the
         * stream (NV12 for 8-bit, P010 for 10-bit HDR, P016 for 12-bit).
         * We then only override the bind flags so the resulting
         * texture-array surfaces are simultaneously decoder targets and
         * shader-sample sources, completing the zero-copy path. */
        AVBufferRef *frames_ref = NULL;
        int ret = avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx,
                                                   *p, &frames_ref);
        if (ret < 0 || !frames_ref) {
            av_log(ctx, AV_LOG_WARNING,
                   "avcodec_get_hw_frames_parameters(%s) failed: %d\n",
                   av_get_pix_fmt_name(*p), ret);
            av_buffer_unref(&frames_ref);
            continue;
        }

        AVHWFramesContext *frames_ctx = (AVHWFramesContext *)frames_ref->data;
        if (frames_ctx->initial_pool_size < 20)
            frames_ctx->initial_pool_size = 20;

#ifdef _WIN32
        if (dctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
            AVD3D11VAFramesContext *d3d_frames =
                (AVD3D11VAFramesContext *)frames_ctx->hwctx;
            /* OR-in the renderer-required flags; preserve any flags the
             * codec already requested for itself. */
            d3d_frames->BindFlags |= D3D11_BIND_DECODER |
                                     D3D11_BIND_SHADER_RESOURCE;
        }
#endif

        if (av_hwframe_ctx_init(frames_ref) < 0) {
            av_log(ctx, AV_LOG_WARNING,
                   "Failed to init HW frames pool (%s, %dx%d sw=%s).\n",
                   av_get_pix_fmt_name(frames_ctx->format),
                   frames_ctx->width, frames_ctx->height,
                   av_get_pix_fmt_name(frames_ctx->sw_format));
            av_buffer_unref(&frames_ref);
            continue;
        }

        av_log(ctx, AV_LOG_INFO,
               "Hardware frames pool ready: %s %dx%d, sw_format=%s.\n",
               av_get_pix_fmt_name(frames_ctx->format),
               frames_ctx->width, frames_ctx->height,
               av_get_pix_fmt_name(frames_ctx->sw_format));

        av_buffer_unref(&ctx->hw_frames_ctx);
        ctx->hw_frames_ctx = frames_ref;
        return *p;
    }

    av_log(ctx, AV_LOG_WARNING,
           "No hardware pixel format from %s could be matched against the "
           "available device context; falling back to software decoding.\n",
           avcodec_get_name(ctx->codec_id));
    return pix_fmts[0];
}

void stream_seek(VideoState *is, int64_t pos, int64_t rel, int by_bytes)
{
    // Throttle multiple seek requests
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

void stream_refresh(VideoState *is, double *remaining_time)
{
    double time;
    Frame *sp, *sp2;

    if (!is->paused && av_sync_is_external_clock_master(&is->av_sync) &&
        demuxer_is_realtime(is->demuxer))
        check_external_clock_speed(&is->av_sync);

    if (is->show_mode != SHOW_MODE_VIDEO && is->audio_st && is->audio_visualizer) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->audio_visualizer->last_vis_time + is->audio_visualizer->rdftspeed < time) {
            is->audio_visualizer->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->audio_visualizer->last_vis_time + is->audio_visualizer->rdftspeed - time);
    }

    if (is->video_st) {
retry:
        if (frame_queue_nb_remaining(is->pictq) == 0) {
        } else {
            double duration, delay;
            Frame *vp, *lastvp;

            lastvp = frame_queue_peek_last(is->pictq);
            vp = frame_queue_peek(is->pictq);

            if (vp->serial != packet_queue_get_serial(is->videoq)) {
                frame_queue_next(is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            delay = av_sync_compute_frame_delay(&is->av_sync, lastvp, vp);

            time= av_gettime_relative()/1000000.0;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (av_sync_should_reset_frame_timer(delay, time, is->frame_timer))
                is->frame_timer = time;

            frame_queue_lock(is->pictq);
            av_sync_update_video_pts_if_valid(&is->av_sync, vp->pts, vp->serial);
            frame_queue_unlock(is->pictq);

            if (frame_queue_nb_remaining(is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(is->pictq);
                duration = vp_duration(&is->av_sync, vp, nextvp);
                if (av_sync_should_late_drop(&is->av_sync, is->step, time, is->frame_timer, duration)) {
                    is->frame_drops_late++;
                    frame_queue_next(is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(is->subpq) > 0) {
                    sp = frame_queue_peek(is->subpq);
                    if (frame_queue_nb_remaining(is->subpq) > 1)
                        sp2 = frame_queue_peek_next(is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != packet_queue_get_serial(is->subtitleq)
                            || (clock_get_pts(is->vidclk) > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                            || (sp2 && clock_get_pts(is->vidclk) > (sp2->pts + ((float) sp2->sub.start_display_time / 1000)))) {
                        frame_queue_next(is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused)
                stream_toggle_pause(is);
        }
display:
        ;
    }
    is->force_refresh = 0;
}

void stream_request_refresh(VideoState *is)
{
    if (!is)
        return;
    is->force_refresh = 1;
}

/* -- Accessors ---------------------------------- */

Demuxer *stream_get_demuxer(const VideoState *is)
{
    return is ? is->demuxer : NULL;
}

int stream_is_paused(const VideoState *is)
{
    return is ? is->paused : 1;
}

int stream_get_volume(const VideoState *is)
{
    if (!is || !is->audio_pipeline)
        return 0;
    return is->audio_pipeline->audio_volume;
}

int stream_needs_refresh(const VideoState *is)
{
    if (!is)
        return 0;
    return is->show_mode != SHOW_MODE_NONE &&
           (!is->paused || is->force_refresh);
}

int stream_is_video_open(const VideoState *is)
{
    return is && is->width > 0;
}

int stream_is_video_decoder_hardware(const VideoState *is)
{
    return is ? is->video_decoder_uses_hw : 0;
}

int stream_has_video_hw_fallback(const VideoState *is)
{
    return is ? is->hw_fallback_triggered : 0;
}

/* -- High-level queries ------------------------ */

void stream_seek_to_ratio(VideoState *is, float ratio)
{
    int64_t ts, size;

    if (!is)
        return;

    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
    if (!ic)
        return;

    ratio = av_clipf(ratio, 0.0f, 1.0f);
    if (demuxer_get_seek_mode(is->demuxer) || ic->duration <= 0) {
        if (!ic->pb)
            return;
        size = avio_size(ic->pb);
        if (size <= 0)
            return;
        stream_seek(is, (int64_t)(size * ratio), 0, 1);
        return;
    }
    ts = (int64_t)(ratio * ic->duration);
    if (ic->start_time != AV_NOPTS_VALUE)
        ts += ic->start_time;
    stream_seek(is, ts, 0, 0);
}

double stream_get_position(const VideoState *is)
{
    double pos;
    if (!is)
        return 0.0;
    pos = get_master_clock(&is->av_sync);
    if (isnan(pos))
        return 0.0;
    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
    if (ic && ic->start_time != AV_NOPTS_VALUE)
        pos -= ic->start_time / (double)AV_TIME_BASE;
    return pos > 0.0 ? pos : 0.0;
}

double stream_get_duration(const VideoState *is)
{
    if (!is)
        return -1.0;
    return demuxer_get_duration_seconds(is->demuxer);
}

int stream_is_eof(const VideoState *is)
{
    if (!is)
        return 0;
    return demuxer_is_eof(is->demuxer);
}

int stream_has_quit_request(const VideoState *is)
{
    if (!is)
        return 0;
    return is->quit_request;
}

int stream_has_chapters(const VideoState *is)
{
    if (!is)
        return 0;
    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
    return ic && ic->nb_chapters > 1;
}

const char *stream_get_media_title(const VideoState *is)
{
    if (!is)
        return NULL;
    return demuxer_get_input_name(is->demuxer);
}

int stream_can_seek(const VideoState *is)
{
    if (!is)
        return 0;
    return demuxer_stream_is_seekable(is->demuxer);
}

float stream_get_byte_progress(const VideoState *is)
{
    if (!is)
        return -1.0f;
    return demuxer_get_byte_progress(is->demuxer);
}

void stream_cycle_audio(VideoState *is)
{
    if (is) stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
}

void stream_cycle_video(VideoState *is)
{
    if (is) stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
}

void stream_cycle_subtitle(VideoState *is)
{
    if (is) stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
}

void stream_handle_window_size_changed(VideoState *is, int width, int height)
{
    if (!is)
        return;
    is->width = width;
    is->height = height;
    audio_visualizer_invalidate_texture(is->audio_visualizer);
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

    if (!is)
        return;
    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
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

    if (!is)
        return;

    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
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

VideoState *stream_open(const char *filename,
                        AudioDevice *audio_device,
                        const enum AVPixelFormat *supported_pix_fmts,
                        int nb_supported_pix_fmts,
                        AVBufferRef *hw_device_ctx,
                        void (*frame_size_changed_cb)(void *opaque, int width, int height, AVRational sar),
                        void *frame_size_opaque)
{
    // TODO: stream_prepare

    if (!filename || !audio_device || !supported_pix_fmts || nb_supported_pix_fmts <= 0)
        return NULL;

    VideoState *is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;

    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->video_decoder_uses_hw = 0;
    is->hw_fallback_triggered = 0;

    is->demuxer = demuxer_create(filename);
    if (!is->demuxer)
        goto fail;

    is->ytop = 0;
    is->xleft = 0;

    is->audio_device = audio_device;
    audio_device_set_open_cb(audio_device, audio_pipeline_open);
    is->nb_supported_pix_fmts = FFMIN(nb_supported_pix_fmts, (int)FF_ARRAY_ELEMS(is->supported_pix_fmts));
    memcpy(is->supported_pix_fmts, supported_pix_fmts, is->nb_supported_pix_fmts * sizeof(is->supported_pix_fmts[0]));
    /* Take our own reference so the caller can release theirs while
     * playback is still alive. We hand out further refs to each
     * AVCodecContext that wants hwaccel. */
    if (hw_device_ctx)
        is->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    is->on_frame_size_changed = frame_size_changed_cb;
    is->frame_size_opaque = frame_size_opaque;
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
    av_sync_bind(&is->av_sync, is->audclk, is->vidclk, is->extclk,
                 is->audioq, is->videoq, &is->audio_st,
                 &is->audio_stream, &is->video_stream,
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
    is->audio_visualizer = audio_visualizer_create();
    if (!is->audio_visualizer)
        goto fail;
    audio_visualizer_bind(is->audio_visualizer, is->audio_pipeline,
                          &is->paused, (int *)&is->show_mode);


    // TODO: stream_start (fire read thread)

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
    AVCodecContext *avctx;
    const AVCodec *codec;
    AVDictionary *opts = NULL;
    int sample_rate;
    AVChannelLayout ch_layout = { 0 };
    int ret = 0;
    
    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
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

    /* Wire up hardware acceleration before opening the codec so that
     * libavcodec's hwaccel lookup picks our get_format hook on the very
     * first packet. The reference is duplicated; each AVCodecContext
     * owns its own ref-count and releases it via avcodec_free_context. */
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO && is->hw_device_ctx) {
        avctx->hw_device_ctx = av_buffer_ref(is->hw_device_ctx);
        if (avctx->hw_device_ctx)
            avctx->get_format = ffplay_get_format;
    }

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
            if ((ret = configure_audio_filters(&is->agraph, &is->audio_filter_src, &is->audio_pipeline->audio_tgt, NULL, 0, &is->in_audio_filter, &is->out_audio_filter)) < 0)
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
        if (ic->iformat && (ic->iformat->flags & AVFMT_NOTIMESTAMPS)) {
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
        is->video_decoder_uses_hw = avctx->hw_device_ctx ? 1 : 0;
        is->hw_fallback_triggered = 0;

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
    AVCodecParameters *codecpar;

    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
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

        audio_visualizer_reset(is->audio_visualizer);
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, is->pictq);
        decoder_destroy(&is->viddec);
        is->video_decoder_uses_hw = 0;
        is->hw_fallback_triggered = 0;
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

int stream_video_reopen_software(VideoState *is)
{
    if (!is || is->video_stream < 0)
        return AVERROR(EINVAL);

    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
    if (!ic || is->video_stream >= (int)ic->nb_streams)
        return AVERROR(EINVAL);

    AVStream *st = ic->streams[is->video_stream];

    /* Build a fresh, software-only decoder context that mirrors what
     * stream_component_open does for video streams, MINUS the hwaccel
     * wiring. We deliberately do not touch is->hw_device_ctx so that a
     * subsequent stream_open (different file) can still try HW first. */
    AVCodecContext *avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    int ret = avcodec_parameters_to_context(avctx, st->codecpar);
    if (ret < 0)
        goto fail;

    avctx->pkt_timebase = st->time_base;

    const AVCodec *codec = avcodec_find_decoder(avctx->codec_id);
    if (!codec) {
        ret = AVERROR(EINVAL);
        goto fail;
    }
    avctx->codec_id = codec->id;
    avctx->lowres   = 0;

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "threads", "auto", 0);
    av_dict_set(&opts, "flags",   "+copy_opaque", AV_DICT_MULTIKEY);

    ret = avcodec_open2(avctx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        goto fail;

    /* Hot-swap. video_thread is the sole consumer of viddec.avctx and
     * is the caller of this function, so no lock is needed. The old
     * context's references (hw_device_ctx + hw_frames_ctx + opened
     * codec internals) are released by avcodec_free_context. */
    avcodec_free_context(&is->viddec.avctx);
    is->viddec.avctx     = avctx;
    is->viddec.finished  = 0;
    is->viddec.packet_pending = 0;
    av_packet_unref(is->viddec.pkt);
    is->video_decoder_uses_hw = 0;
    is->hw_fallback_triggered = 1;

    av_log(NULL, AV_LOG_WARNING,
           "Video decoder hot-swapped to software path (codec=%s).\n",
           avcodec_get_name(avctx->codec_id));
    return 0;

fail:
    avcodec_free_context(&avctx);
    return ret;
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
    audio_visualizer_free(&is->audio_visualizer);
    audio_pipeline_free(&is->audio_pipeline);
    clock_destroy(&is->audclk);
    clock_destroy(&is->vidclk);
    clock_destroy(&is->extclk);
    av_buffer_unref(&is->hw_device_ctx);
    av_free(is);
}

void stream_cycle_channel(VideoState *is, int codec_type)
{
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams;

    AVFormatContext *ic = demuxer_get_format_context(is->demuxer);
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

/* -- Frame access (pull-based) ------------------ */

AudioVisualizer *stream_get_audio_visualizer(const VideoState *is)
{
    return (is && is->audio_st) ? is->audio_visualizer : NULL;
}

AVFrame *stream_get_video_frame(const VideoState *is)
{
    if (!is || !is->video_st)
        return NULL;
    if (!frame_queue_is_last_shown(is->pictq))
        return NULL;
    Frame *vp = frame_queue_peek_last(is->pictq);
    return vp ? vp->frame : NULL;
}

AVSubtitle *stream_get_subtitle(const VideoState *is)
{
    Frame *vp, *sp;
    if (!is || !is->subtitle_st || !is->video_st)
        return NULL;
    if (frame_queue_nb_remaining(is->subpq) <= 0)
        return NULL;
    if (!frame_queue_is_last_shown(is->pictq))
        return NULL;
    vp = frame_queue_peek_last(is->pictq);
    sp = frame_queue_peek(is->subpq);
    if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000))
        return &sp->sub;
    return NULL;
}

int stream_get_video_size(const VideoState *is, int *width, int *height, AVRational *sar)
{
    Frame *vp;
    if (!is || !is->video_st)
        return -1;
    if (!frame_queue_is_last_shown(is->pictq))
        return -1;
    vp = frame_queue_peek_last(is->pictq);
    if (!vp || !vp->width || !vp->height)
        return -1;
    if (width)  *width  = vp->width;
    if (height) *height = vp->height;
    if (sar)    *sar    = vp->sar;
    return 0;
}

int stream_get_show_mode(const VideoState *is)
{
    if (!is)
        return -1;
    return (int)is->show_mode;
}

void stream_set_window_size(VideoState *is, int width, int height)
{
    if (!is)
        return;
    is->width  = width;
    is->height = height;
}
