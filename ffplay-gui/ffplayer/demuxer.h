/**
 * Demux session state wrapping AVFormatContext (same style as Decoder: struct is exposed).
 */

#ifndef FFPLAY_GUI_DEMUXER_H
#define FFPLAY_GUI_DEMUXER_H

#include <stdint.h>

#include <SDL_thread.h>

#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/rational.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @param metadata stream metadata snapshot; may be NULL. Do not hold past callback return. */
typedef void (*DemuxerStreamMetadataFn)(void *opaque, int stream_index,
                                         const AVDictionary *metadata);

typedef struct Demuxer {
    AVFormatContext *ic;

    /** -1 = not decided yet; once set, matches demuxer_should_use_byte_seek() (0 = time-based, 1 = byte). */
    int     seek_mode;
    /** 1 => interrupt blocking demux I/O (stream_close sets before demuxer_stop). */
    int     abort_request;
    int     eof;
    double  max_frame_duration;
    char   *input_url;

    SDL_Thread *read_tid;
    SDL_mutex  *wait_mutex;
    SDL_cond   *continue_read_thread;

    int queue_attachments_req;
    int read_pause_return;

    int st_index[AVMEDIA_TYPE_NB];

    /** Optional; called when a packet clears AVSTREAM_EVENT_FLAG_METADATA_UPDATED for that stream. */
    DemuxerStreamMetadataFn on_stream_metadata;
    void                   *stream_metadata_opaque;
} Demuxer;

/**
 * Initialise demuxer shell: allocates AVFormatContext, interrupt callback,
 * mutex/cond for the read loop, copies @p input_url. Does not open the input.
 */
int demuxer_init(Demuxer *d, const char *input_url);

/**
 * Tear down resources. Call demuxer_stop() first after the read thread was started.
 * After return, treat @p d as uninitialized.
 */
void demuxer_destroy(Demuxer *d);

/** Stream index for @p type, or -1 if absent; invalid @p type yields -1. */
int demuxer_stream_index(const Demuxer *d, enum AVMediaType type);

/**
 * Format open options: same semantics as libavformat avformat_open_input @p options.
 * If @p format_opts is NULL, an internal transient dictionary is used (callers stay unchanged).
 * If non-NULL, *format_opts may be empty; merged with scan_all_pmts unless already set,
 * passed to open_input as in/out, then unrecognized keys remain in *format_opts.
 */
int demuxer_open_input(Demuxer *d, AVDictionary **format_opts);

/**
 * Probe streams: wrappers avformat_find_stream_info.
 * If @p stream_opts is NULL, probing uses no per-stream options (current callers).
 * If non-NULL, must point at an array of ic->nb_streams pointers (each may be NULL),
 * matching FFmpeg's avformat_find_stream_info second argument.
 */
int demuxer_find_stream_info(Demuxer *d, AVDictionary **stream_opts);

void demuxer_io_reset_eof(Demuxer *d);

void demuxer_set_stream_metadata_fn(Demuxer *d, DemuxerStreamMetadataFn fn, void *opaque);

int    demuxer_should_use_byte_seek(const Demuxer *d);
double demuxer_get_max_gap(const Demuxer *d);
int    demuxer_is_realtime(const Demuxer *d);
int    demuxer_find_stream_components(Demuxer *d);
int    demuxer_is_realtime_network_protocol(const Demuxer *d);

int  demuxer_read_packet(Demuxer *d, AVPacket *pkt);
int  demuxer_is_io_error(const Demuxer *d);
int  demuxer_should_handle_eof(const Demuxer *d, int read_ret);

/** For each pkt: if FFmpeg marked metadata updated on that stream, notify then clear flag. */
void demuxer_handle_pkt_stream_events(Demuxer *d, AVPacket *pkt);

int demuxer_remote_play(Demuxer *d);
int demuxer_remote_pause(Demuxer *d);

int        demuxer_get_stream_width(const Demuxer *d, int stream_index);
int        demuxer_get_stream_height(const Demuxer *d, int stream_index);
AVRational demuxer_guess_sample_aspect_ratio(const Demuxer *d, int stream_index, AVFrame *frame);
AVRational demuxer_guess_frame_rate(const Demuxer *d, int stream_index, AVFrame *frame);

int    demuxer_seek_file(Demuxer *d, int stream_index,
                         int64_t min_ts, int64_t ts, int64_t max_ts, int flags);
double demuxer_get_duration_seconds(const Demuxer *d);
int    demuxer_stream_is_seekable(const Demuxer *d);
float  demuxer_get_byte_progress(const Demuxer *d);

int  demuxer_start(Demuxer *d, int (*read_thread_fn)(void *), void *arg);
void demuxer_stop(Demuxer *d);

void demuxer_notify_continue_read(Demuxer *d);
void demuxer_wait_for_continue_reading(Demuxer *d, int32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
