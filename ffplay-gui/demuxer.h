#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include "libavformat/avformat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct VideoState;

typedef struct Demuxer {
    AVFormatContext *ic;
    int seek_mode;
} Demuxer;

int demuxer_get_seek_mode(const struct VideoState *is);
const char *demuxer_get_input_name(const struct VideoState *is);

int read_thread(void *arg);

#ifdef __cplusplus
}
#endif

#endif
