#ifndef FFPLAY_GUI_DECODER_H
#define FFPLAY_GUI_DECODER_H

#include "frame_queue.h"
#include <SDL.h>
#include <SDL_thread.h>

#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue FrameQueue;

typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

typedef struct Decoder {
    AVPacket       *pkt;
    PacketQueue    *queue;
    AVCodecContext *avctx;
    int             pkt_serial;
    int             finished;
    int             packet_pending;
    SDL_cond       *empty_queue_cond;
    int64_t         start_pts;
    AVRational      start_pts_tb;
    int64_t         next_pts;
    AVRational      next_pts_tb;
    SDL_Thread     *decoder_tid;
} Decoder;

/**
 * Initialize the decoder context.
 */
int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);

/**
 * Decode one frame and return it.
 * The decoded frame will be stored in the provided AVFrame and AVSubtitle structures.
 */
int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);

/**
 * Destroy the decoder context.
 */
void decoder_destroy(Decoder *d);

/**
 * Abort the decoder.
 */
void decoder_abort(Decoder *d, FrameQueue *fq);

/**
 * Start the decoder thread.
 */
int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg);

#ifdef __cplusplus
}
#endif

#endif
