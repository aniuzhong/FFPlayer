#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include "libavformat/avformat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Demuxer {
    AVFormatContext *ic;
    int seek_mode;
    int abort_request;
    char *input_url;
} Demuxer;

int demuxer_init(Demuxer *demuxer, const char *input_url);
void demuxer_destroy(Demuxer *demuxer);
void demuxer_request_abort(Demuxer *demuxer);
int demuxer_is_aborted(const Demuxer *demuxer);
int demuxer_get_seek_mode(const Demuxer *demuxer);
const char *demuxer_get_input_name(const Demuxer *demuxer);

#ifdef __cplusplus
}
#endif

#endif
