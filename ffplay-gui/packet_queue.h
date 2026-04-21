#ifndef FFPLAY_GUI_PACKET_QUEUE_H
#define FFPLAY_GUI_PACKET_QUEUE_H

#include <SDL.h>
#include <SDL_thread.h>

#include <libavutil/fifo.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MyAVPacketList {
    AVPacket *pkt;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    AVFifo    *pkt_list;      // FIFO storage of queued packets
    int        nb_packets;    // Number of packets in queue
    int        size;          // Total queued size in bytes
    int64_t    duration;      // Total queued duration in stream time base
    int        abort_request; // Abort flag for blocking operations
    int        serial;        // Queue generation, bumped on flush/start
    SDL_mutex *mutex;         // Mutex protecting queue state
    SDL_cond  *cond;          // Condition variable for wait/signal
} PacketQueue;

int  packet_queue_put(PacketQueue *q, AVPacket *pkt);
int  packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index);
int  packet_queue_init(PacketQueue *q);
void packet_queue_flush(PacketQueue *q);
void packet_queue_destroy(PacketQueue *q);
void packet_queue_abort(PacketQueue *q);
void packet_queue_start(PacketQueue *q);
int  packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

#ifdef __cplusplus
}
#endif

#endif
