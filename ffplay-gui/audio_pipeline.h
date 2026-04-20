#ifndef FFPLAY_GUI_AUDIO_PIPELINE_H
#define FFPLAY_GUI_AUDIO_PIPELINE_H

#include "video_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int audio_pipeline_open(void *opaque,
                        AVChannelLayout *wanted_channel_layout,
                        int wanted_sample_rate,
                        struct AudioParams *audio_hw_params);

#ifdef __cplusplus
}
#endif

#endif
