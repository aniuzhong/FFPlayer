#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Demuxer {
    AVFormatContext *ic;
    int              seek_mode;
    int              abort_request;
    int              realtime;
    int              eof;
    double           max_frame_duration;
    char            *input_url;
} Demuxer;

int demuxer_init(Demuxer *demuxer, const char *input_url);
void demuxer_destroy(Demuxer *demuxer);
void demuxer_request_abort(Demuxer *demuxer);
int demuxer_is_aborted(const Demuxer *demuxer);
int demuxer_get_seek_mode(const Demuxer *demuxer);
const char *demuxer_get_input_name(const Demuxer *demuxer);
int demuxer_is_realtime(const Demuxer *demuxer);
int demuxer_is_eof(const Demuxer *demuxer);
double demuxer_get_max_frame_duration(const Demuxer *demuxer);
void demuxer_set_realtime(Demuxer *demuxer, int realtime);
void demuxer_set_eof(Demuxer *demuxer, int eof);
void demuxer_set_max_frame_duration(Demuxer *demuxer, double max_frame_duration);

#ifdef __cplusplus
}
#endif

#endif
