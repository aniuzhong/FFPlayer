#ifndef FFPLAY_GUI_AUDIO_PIPELINE_H
#define FFPLAY_GUI_AUDIO_PIPELINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

#include "audio_device.h"

#define SAMPLE_ARRAY_SIZE (8 * 65536)

enum ShowMode {
    SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
};

/* Forward declarations for borrowed references */
typedef struct AvSync AvSync;
typedef struct FrameQueue FrameQueue;
typedef struct PacketQueue PacketQueue;

typedef struct AudioPipeline {
    /* Borrowed references (not owned, set via audio_pipeline_bind) */
    AvSync *av_sync;
    FrameQueue *sampq;
    PacketQueue *audioq;
    AudioDevice *audio_device;
    int *paused;
    int *show_mode;

    /* Pipeline control */
    int muted;
    int audio_volume;

    /* A/V sync compensation state */
    double audio_diff_cum;
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;

    /* Audio clock state */
    double audio_clock;
    int audio_clock_serial;
    int64_t audio_callback_time;

    /* Audio device output parameters */
    struct AudioParams audio_src;
    struct AudioParams audio_tgt;
    int audio_hw_buf_size;

    /* Resample context and buffer */
    struct SwrContext *swr_ctx;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size;
    unsigned int audio_buf1_size;
    int audio_buf_index;
    int audio_write_buf_size;

    /* Visualization sample buffer */
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
} AudioPipeline;

AudioPipeline *audio_pipeline_create(void);
void audio_pipeline_free(AudioPipeline **ap);

void audio_pipeline_bind(AudioPipeline *ap, AvSync *av_sync, FrameQueue *sampq,
                         PacketQueue *audioq, AudioDevice *audio_device, int *paused, int *show_mode);

int audio_pipeline_open(void *opaque, AVChannelLayout *wanted_channel_layout,
                         int wanted_sample_rate, struct AudioParams *audio_hw_params);

/* Initialize sync compensation state after device open */
void audio_pipeline_init_sync(AudioPipeline *ap);

/* Release resample context and buffers (called on stream close) */
void audio_pipeline_reset(AudioPipeline *ap);

#ifdef __cplusplus
}
#endif

#endif
