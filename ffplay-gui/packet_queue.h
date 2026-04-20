#ifndef FFPLAY_GUI_PACKET_QUEUE_H
#define FFPLAY_GUI_PACKET_QUEUE_H

#include <SDL.h>
#include <SDL_thread.h>

#include "libavutil/fifo.h"
#include "libavcodec/avcodec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo *pkt_list;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index);
int packet_queue_init(PacketQueue *q);
void packet_queue_flush(PacketQueue *q);
void packet_queue_destroy(PacketQueue *q);
void packet_queue_abort(PacketQueue *q);
void packet_queue_start(PacketQueue *q);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

#ifdef __cplusplus
}
#endif

#endif
