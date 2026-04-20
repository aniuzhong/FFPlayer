#ifndef FFPLAY_GUI_FILTER_H
#define FFPLAY_GUI_FILTER_H

#include "video_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame, const SDL_RendererInfo *renderer_info);
int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format);

#ifdef __cplusplus
}
#endif

#endif
