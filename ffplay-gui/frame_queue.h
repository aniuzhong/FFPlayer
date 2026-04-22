/*
 * Frame queue implementation.
 *
 * Add getters:
 * - frame_queue_is_initialized
 * - frame_queue_is_last_shown
 * 
 * Add locking primitives:
 * - frame_queue_lock
 * - frame_queue_unlock
 */

#ifndef FFPLAY_GUI_FRAME_QUEUE_H
#define FFPLAY_GUI_FRAME_QUEUE_H

#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue FrameQueue;

typedef struct Frame {
    AVFrame     *frame;
    AVSubtitle   sub;
    int          serial; // Frame serial number
    double       pts; // Presentation timestamp
    double       duration; // Estimated duration of the frame
    int64_t      pos; // Byte position of the frame in the input file   
    int          width; // Frame width
    int          height; // Frame height
    int          format; // Frame format
    AVRational   sar; // Sample aspect ratio
    int          uploaded; // Flag indicating if the frame has been uploaded
    int          flip_v; // Flag indicating if the frame should be flipped vertically
} Frame;

typedef struct FrameQueue FrameQueue;

/**
 * Initialize frame queue resources and state.
 */
int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);

/**
 * Allocate and initialize a frame queue instance.
 */
FrameQueue *frame_queue_create(PacketQueue *pktq, int max_size, int keep_last);

/**
 * Destroy frame queue resources and release allocated frames.
 */
void frame_queue_destroy(FrameQueue *f);

/**
 * Destroy and free a frame queue instance.
 */
void frame_queue_free(FrameQueue **f);

/**
 * Signal one waiter blocked on frame queue condition.
 */
void frame_queue_signal(FrameQueue *f);

/**
 * Lock frame queue mutex.
 */
void frame_queue_lock(FrameQueue *f);

/**
 * Unlock frame queue mutex.
 */
void frame_queue_unlock(FrameQueue *f);

/**
 * Peek current readable frame (respecting keep_last state).
 */
Frame *frame_queue_peek(FrameQueue *f);

/**
 * Peek next frame after current readable frame.
 */
Frame *frame_queue_peek_next(FrameQueue *f);

/**
 * Peek the last displayed frame slot.
 */
Frame *frame_queue_peek_last(FrameQueue *f);

/**
 * Wait for and return writable frame slot; returns NULL on abort.
 */
Frame *frame_queue_peek_writable(FrameQueue *f);

/**
 * Wait for and return readable frame slot; returns NULL on abort.
 */
Frame *frame_queue_peek_readable(FrameQueue *f);

/**
 * Advance write index and publish one queued frame.
 */
void frame_queue_push(FrameQueue *f);

/**
 * Advance read index and release one consumed frame.
 */
void frame_queue_next(FrameQueue *f);

/**
 * Return number of remaining readable frames.
 */
int frame_queue_nb_remaining(FrameQueue *f);

/**
 * Return last shown frame position if serial matches current packet queue.
 */
int64_t frame_queue_last_pos(FrameQueue *f);

/**
 * Return non-zero if queue synchronization primitives are initialized.
 */
int frame_queue_is_initialized(const FrameQueue *f);

/**
 * Return non-zero if keep_last frame is currently shown.
 */
int frame_queue_is_last_shown(const FrameQueue *f);

#ifdef __cplusplus
}
#endif

#endif
