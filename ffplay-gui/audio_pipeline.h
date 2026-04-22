#ifndef FFPLAY_GUI_AUDIO_PIPELINE_H
#define FFPLAY_GUI_AUDIO_PIPELINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libavutil/channel_layout.h>

#include "audio_device.h"

int audio_pipeline_open(void *opaque, AVChannelLayout *wanted_channel_layout, int wanted_sample_rate, struct AudioParams *audio_hw_params);

#ifdef __cplusplus
}
#endif

#endif
