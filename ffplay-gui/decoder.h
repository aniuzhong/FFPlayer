#ifndef FFPLAY_GUI_DECODER_H
#define FFPLAY_GUI_DECODER_H

#include "frame_queue.h"

#include "libavutil/rational.h"
#include "libavcodec/avcodec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameData {
    int64_t pkt_pos;
} FrameData;

typedef struct Decoder {
    AVPacket *pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

int decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond);
int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);
void decoder_destroy(Decoder *d);
void decoder_abort(Decoder *d, FrameQueue *fq);
int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg);

#ifdef __cplusplus
}
#endif

#endif
