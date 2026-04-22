#ifndef FFPLAY_GUI_READ_THREAD_H
#define FFPLAY_GUI_READ_THREAD_H

#include "video_state.h"

#ifdef __cplusplus
extern "C" {
#endif

int read_thread(void *arg);
int find_stream_components(VideoState *is);

#ifdef __cplusplus
}
#endif

#endif
