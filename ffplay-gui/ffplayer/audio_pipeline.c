#include <math.h>
#include <string.h>

#include <libavutil/common.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#include "audio_pipeline.h"
#include "audio_device.h"
#include "av_sync.h"
#include "packet_queue.h"

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB   20

/* copy samples for viewing in editor window */
static void update_sample_display(AudioPipeline *ap, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - ap->sample_array_index;
        if (len > size)
            len = size;
        memcpy(ap->sample_array + ap->sample_array_index, samples, len * sizeof(short));
        samples += len;
        ap->sample_array_index += len;
        if (ap->sample_array_index >= SAMPLE_ARRAY_SIZE)
            ap->sample_array_index = 0;
        size -= len;
    }
}

static int synchronize_audio(AudioPipeline *ap, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    if (!av_sync_is_audio_master(ap->av_sync)) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = av_sync_audio_master_diff(ap->av_sync);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            ap->audio_diff_cum = diff + ap->audio_diff_avg_coef * ap->audio_diff_cum;
            if (ap->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                ap->audio_diff_avg_count++;
            } else {
                avg_diff = ap->audio_diff_cum * (1.0 - ap->audio_diff_avg_coef);

                if (fabs(avg_diff) >= ap->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * ap->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        ap->audio_clock, ap->audio_diff_threshold);
            }
        } else {
            ap->audio_diff_avg_count = 0;
            ap->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

static int audio_decode_frame(AudioPipeline *ap)
{
    int data_size, resampled_data_size;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (*ap->paused)
        return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(ap->sampq) == 0) {
            if ((av_gettime_relative() - ap->audio_callback_time) > 1000000LL * ap->audio_hw_buf_size / ap->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(ap->sampq)))
            return -1;
        frame_queue_next(ap->sampq);
    } while (af->serial != packet_queue_get_serial(ap->audioq));

    data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                                           af->frame->nb_samples,
                                           (enum AVSampleFormat)af->frame->format, 1);

    wanted_nb_samples = synchronize_audio(ap, af->frame->nb_samples);

    if (af->frame->format        != ap->audio_src.fmt            ||
        av_channel_layout_compare(&af->frame->ch_layout, &ap->audio_src.ch_layout) ||
        af->frame->sample_rate   != ap->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !ap->swr_ctx)) {
        int ret;
        swr_free(&ap->swr_ctx);
        ret = swr_alloc_set_opts2(&ap->swr_ctx,
                                  &ap->audio_tgt.ch_layout, ap->audio_tgt.fmt, ap->audio_tgt.freq,
                                  &af->frame->ch_layout, (enum AVSampleFormat)af->frame->format, af->frame->sample_rate,
                                  0, NULL);
        if (ret < 0 || swr_init(ap->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name((enum AVSampleFormat)af->frame->format), af->frame->ch_layout.nb_channels,
                   ap->audio_tgt.freq, av_get_sample_fmt_name(ap->audio_tgt.fmt), ap->audio_tgt.ch_layout.nb_channels);
            swr_free(&ap->swr_ctx);
            return -1;
        }
        if (av_channel_layout_copy(&ap->audio_src.ch_layout, &af->frame->ch_layout) < 0)
            return -1;
        ap->audio_src.freq = af->frame->sample_rate;
        ap->audio_src.fmt = (enum AVSampleFormat)af->frame->format;
    }

    if (ap->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &ap->audio_buf1;
        int out_count = (int64_t)wanted_nb_samples * ap->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size  = av_samples_get_buffer_size(NULL, ap->audio_tgt.ch_layout.nb_channels, out_count, ap->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(ap->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * ap->audio_tgt.freq / af->frame->sample_rate,
                                     wanted_nb_samples * ap->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&ap->audio_buf1, &ap->audio_buf1_size, out_size);
        if (!ap->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(ap->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(ap->swr_ctx) < 0)
                swr_free(&ap->swr_ctx);
        }
        ap->audio_buf = ap->audio_buf1;
        resampled_data_size = len2 * ap->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(ap->audio_tgt.fmt);
    } else {
        ap->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = ap->audio_clock;
    if (!isnan(af->pts))
        ap->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        ap->audio_clock = NAN;
    ap->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               ap->audio_clock - last_clock,
               ap->audio_clock, audio_clock0);
        last_clock = ap->audio_clock;
    }
#endif
    return resampled_data_size;
}

static void audio_pipeline_sdl_callback(void *opaque, Uint8 *stream, int len)
{
    AudioPipeline *ap = (AudioPipeline *)opaque;
    int audio_size, len1;

    ap->audio_callback_time = av_gettime_relative();

    while (len > 0) {
        if (ap->audio_buf_index >= ap->audio_buf_size) {
            audio_size = audio_decode_frame(ap);
            if (audio_size < 0) {
                ap->audio_buf = NULL;
                ap->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / ap->audio_tgt.frame_size * ap->audio_tgt.frame_size;
            } else {
                if (*ap->show_mode != SHOW_MODE_VIDEO)
                    update_sample_display(ap, (int16_t *)ap->audio_buf, audio_size);
                ap->audio_buf_size = audio_size;
            }
            ap->audio_buf_index = 0;
        }
        len1 = ap->audio_buf_size - ap->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!ap->muted && ap->audio_buf && ap->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)ap->audio_buf + ap->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!ap->muted && ap->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)ap->audio_buf + ap->audio_buf_index, AUDIO_S16SYS, len1, ap->audio_volume);
        }
        len -= len1;
        stream += len1;
        ap->audio_buf_index += len1;
    }
    ap->audio_write_buf_size = ap->audio_buf_size - ap->audio_buf_index;
    if (!isnan(ap->audio_clock)) {
        av_sync_update_audclk_from_callback(ap->av_sync,
                                            ap->audio_clock,
                                            ap->audio_clock_serial,
                                            ap->audio_hw_buf_size,
                                            ap->audio_write_buf_size,
                                            ap->audio_tgt.bytes_per_sec,
                                            ap->audio_callback_time);
        av_sync_sync_extclk_to_audclk(ap->av_sync);
    }
}

AudioPipeline *audio_pipeline_create(void)
{
    return av_mallocz(sizeof(AudioPipeline));
}

void audio_pipeline_free(AudioPipeline **ap)
{
    AudioPipeline *p;
    if (!ap || !*ap)
        return;
    p = *ap;
    swr_free(&p->swr_ctx);
    av_freep(&p->audio_buf1);
    av_channel_layout_uninit(&p->audio_src.ch_layout);
    av_channel_layout_uninit(&p->audio_tgt.ch_layout);
    av_free(p);
    *ap = NULL;
}

void audio_pipeline_bind(AudioPipeline *ap, AVSync *av_sync, FrameQueue *sampq,
                         PacketQueue *audioq, AudioDevice *audio_device,
                         int *paused, int *show_mode)
{
    if (!ap)
        return;
    ap->av_sync = av_sync;
    ap->sampq = sampq;
    ap->audioq = audioq;
    ap->audio_device = audio_device;
    ap->paused = paused;
    ap->show_mode = show_mode;
}

int audio_pipeline_open(void *opaque,
                        AVChannelLayout *wanted_channel_layout,
                        int wanted_sample_rate,
                        struct AudioParams *audio_hw_params)
{
    AudioPipeline *ap = (AudioPipeline *)opaque;
    if (!ap || !ap->audio_device)
        return -1;
    return audio_device_open_sdl(ap->audio_device, opaque, wanted_channel_layout, wanted_sample_rate, audio_pipeline_sdl_callback, audio_hw_params);
}

void audio_pipeline_init_sync(AudioPipeline *ap)
{
    if (!ap)
        return;
    ap->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
    ap->audio_diff_avg_count = 0;
    ap->audio_diff_threshold = (double)(ap->audio_hw_buf_size) / ap->audio_tgt.bytes_per_sec;
}

void audio_pipeline_reset(AudioPipeline *ap)
{
    if (!ap)
        return;
    swr_free(&ap->swr_ctx);
    av_freep(&ap->audio_buf1);
    ap->audio_buf1_size = 0;
    ap->audio_buf = NULL;
}
