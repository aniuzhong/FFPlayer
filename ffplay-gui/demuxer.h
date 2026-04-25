#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include <SDL_thread.h>

#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Demuxer Demuxer;

/*
 * Create a new Demuxer for the given input URL.
 */
Demuxer *demuxer_create(const char *input_url);

/*
 * Free the resources associated with a Demuxer.
 */
void demuxer_free(Demuxer **demuxer);

/*
 * Getters and setters for Demuxer state.
 */
AVFormatContext *demuxer_get_format_context(const Demuxer *demuxer);
const char      *demuxer_get_input_name(const Demuxer *demuxer);
int              demuxer_is_eof(const Demuxer *demuxer);
void             demuxer_set_eof(Demuxer *demuxer, int eof);
int              demuxer_get_seek_mode(const Demuxer *demuxer);
void             demuxer_set_seek_mode(Demuxer *demuxer, int seek_mode);

/*
 * avformat_open_input() wrapper. Currently `options` are unused.
 */
int demuxer_open_input(Demuxer *demuxer, AVDictionary **options);

/*
 * avformat_find_stream_info() wrapper. Currently `options` are unused.
 */
int demuxer_find_stream_info(Demuxer *demuxer, AVDictionary **options);

/*
 * Set AVFormatContext::AVIOContext::eof_reached.
 */
void demuxer_set_io_context_eof(Demuxer *demuxer, int eof);

/*
 * Determines if byte-based seeking is recommended for the current format.
 */
int demuxer_should_use_byte_seek(Demuxer* demuxer);

/* Thread management */
int demuxer_start(Demuxer *demuxer, int (*read_thread_fn)(void *), void *arg);
void demuxer_stop(Demuxer *demuxer);
void demuxer_notify_continue_read(Demuxer *demuxer);

/* State accessors - abort */
void demuxer_request_abort(Demuxer *demuxer);
int demuxer_is_aborted(const Demuxer *demuxer);

/* State accessors - realtime */
int demuxer_is_realtime(const Demuxer *demuxer);
void demuxer_set_realtime(Demuxer *demuxer, int realtime);

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
