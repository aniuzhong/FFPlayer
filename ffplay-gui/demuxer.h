#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include <SDL_thread.h>

#include <libavformat/avformat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Demuxer Demuxer;

/**
 * Create a new Demuxer for the given input URL.
 */
Demuxer *demuxer_create(const char *input_url);

/**
 * Free the resources associated with a Demuxer.
 */
void demuxer_free(Demuxer **demuxer);

/**
 * Getters and setters for Demuxer state.
 */
AVFormatContext *demuxer_get_format_context(const Demuxer *demuxer);
const char      *demuxer_get_input_name(const Demuxer *demuxer);
int              demuxer_is_eof(const Demuxer *demuxer);
void             demuxer_set_eof(Demuxer *demuxer, int eof);
int              demuxer_get_seek_mode(const Demuxer *demuxer);
void             demuxer_set_seek_mode(Demuxer *demuxer, int seek_mode);
void             demuxer_request_abort(Demuxer *demuxer);
int              demuxer_is_aborted(const Demuxer *demuxer);
double           demuxer_get_max_frame_duration(const Demuxer *demuxer);
void             demuxer_set_max_frame_duration(Demuxer *demuxer, double max_frame_duration);
double *         demuxer_get_max_frame_duration_ptr(Demuxer *demuxer);
SDL_cond *       demuxer_get_continue_read_thread(const Demuxer *demuxer);
int              demuxer_get_queue_attachments_req(const Demuxer *demuxer);
void             demuxer_set_queue_attachments_req(Demuxer *demuxer, int req);
int              demuxer_get_read_pause_return(const Demuxer *demuxer);
void             demuxer_set_read_pause_return(Demuxer *demuxer, int ret);

/**
 * avformat_open_input() wrapper. Currently `options` are unused.
 */
int demuxer_open_input(Demuxer *demuxer, AVDictionary **options);

/**
 * avformat_find_stream_info() wrapper. Currently `options` are unused.
 */
int demuxer_find_stream_info(Demuxer *demuxer, AVDictionary **options);

/**
 * Reset AVFormatContext::AVIOContext::eof_reached.
 */
void demuxer_io_reset_eof(Demuxer *demuxer);

/**
 * Determines if byte-based seeking is recommended for the current format.
 */
int demuxer_should_use_byte_seek(Demuxer* demuxer);

/**
 * Determine the maximum allowed duration between frames based on format stability.
 * For unstable formats (TS), we use a short threshold (10s) to recover quickly.
 * For stable formats, we allow long gaps (1 hour).
 */
double demuxer_get_max_gap(Demuxer* demuxer);

/*
 * Check if the input format is realtime (e.g. RTSP, RTP, SDP).
 */
int demuxer_is_realtime(Demuxer *demuxer);

/**
 * av_find_best_stream() wrapper to find the best stream of each media type and store
 */
int demuxer_find_stream_components(Demuxer *demuxer);

/**
 * Get the stream index for a specific media type.
 */
int demuxer_get_stream_index(const Demuxer *d, enum AVMediaType type);

/**
 * Checks if the protocol is a real-time network type (now RTSP/MMSH)
 * that requires active polling/delaying during pause.
 */
int demuxer_is_realtime_network_protocol(Demuxer *d);

/**
 * Start read_thread
 */
int demuxer_start(Demuxer *demuxer, int (*read_thread_fn)(void *), void *arg);

/**
 * Stop the read_thread and wait for it to finish.
 */
void demuxer_stop(Demuxer *demuxer);

/**
 * Signal the read thread to continue reading.
 */
void demuxer_notify_continue_read(Demuxer *demuxer);

/**
 * Wait for a signal to continue reading. This is used to implement efficient waiting
 */
void demuxer_wait_for_continue_reading(Demuxer *d, int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
