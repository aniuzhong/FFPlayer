#ifndef FFPLAY_GUI_READ_THREAD_H
#define FFPLAY_GUI_READ_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoState VideoState;

int read_thread(void *arg);
int find_stream_components(VideoState *is);

#ifdef __cplusplus
}
#endif

#endif
