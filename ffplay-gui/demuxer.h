#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include "video_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Demuxer {
    VideoState *is;
    int seek_mode;
    const char *input_filename;
    void (*step_to_next_frame_cb)(VideoState *is);
} Demuxer;

Demuxer *demuxer_create(
    VideoState *is,
    const char *input_filename,
    void (*step_to_next_frame_cb)(VideoState *is));

void demuxer_destroy(Demuxer **demuxer);
int demuxer_get_seek_mode(const Demuxer *demuxer);
const char *demuxer_get_input_name(const Demuxer *demuxer);

int read_thread(void *arg);

#ifdef __cplusplus
}
#endif

#endif
