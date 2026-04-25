#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include <libavformat/avformat.h>
#include <SDL_thread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque pointer to Demuxer structure */
typedef struct Demuxer Demuxer;

/* Lifecycle management */
Demuxer *demuxer_create(const char *input_url);
void demuxer_free(Demuxer **demuxer);

/* Thread management */
int demuxer_start(Demuxer *demuxer, int (*read_thread_fn)(void *), void *arg);
void demuxer_stop(Demuxer *demuxer);
void demuxer_notify_continue_read(Demuxer *demuxer);

/* State accessors - abort */
void demuxer_request_abort(Demuxer *demuxer);
int demuxer_is_aborted(const Demuxer *demuxer);

/* State accessors - seek mode */
int demuxer_get_seek_mode(const Demuxer *demuxer);
void demuxer_set_seek_mode(Demuxer *demuxer, int seek_mode);

/* State accessors - input URL */
const char *demuxer_get_input_name(const Demuxer *demuxer);

/* State accessors - format context */
AVFormatContext *demuxer_get_format_context(const Demuxer *demuxer);
void demuxer_set_ic(Demuxer *demuxer, AVFormatContext *ic);

/* State accessors - realtime */
int demuxer_is_realtime(const Demuxer *demuxer);
void demuxer_set_realtime(Demuxer *demuxer, int realtime);

/* State accessors - EOF */
int demuxer_is_eof(const Demuxer *demuxer);
void demuxer_set_eof(Demuxer *demuxer, int eof);

/* State accessors - frame duration */
double demuxer_get_max_frame_duration(const Demuxer *demuxer);
void demuxer_set_max_frame_duration(Demuxer *demuxer, double max_frame_duration);
double *demuxer_get_max_frame_duration_ptr(Demuxer *demuxer);

/* State accessors - read thread condition */
SDL_cond *demuxer_get_continue_read_thread(const Demuxer *demuxer);

/* State accessors - queue attachments */
int demuxer_get_queue_attachments_req(const Demuxer *demuxer);
void demuxer_set_queue_attachments_req(Demuxer *demuxer, int req);

/* State accessors - read pause return */
int demuxer_get_read_pause_return(const Demuxer *demuxer);
void demuxer_set_read_pause_return(Demuxer *demuxer, int ret);

/* Utility function */
int is_realtime(AVFormatContext *s);

#ifdef __cplusplus
}
#endif

#endif
