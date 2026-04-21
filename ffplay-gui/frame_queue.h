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
 *
 */

#ifndef FFPLAY_GUI_FRAME_QUEUE_H
#define FFPLAY_GUI_FRAME_QUEUE_H

#include "packet_queue.h"

#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;
    double duration;
    int64_t pos;
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
} FrameQueue;

/**
 * Initialize frame queue resources and state.
 */
int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last);

/**
 * Destroy frame queue resources and release allocated frames.
 */
void frame_queue_destroy(FrameQueue *f);

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
