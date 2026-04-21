#ifndef FFPLAY_GUI_PACKET_QUEUE_H
#define FFPLAY_GUI_PACKET_QUEUE_H

#include <SDL.h>
#include <SDL_thread.h>

#include <libavutil/fifo.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * Enqueue one packet into the queue.
 */
int  packet_queue_put(PacketQueue *q, AVPacket *pkt);

/**
 * Enqueue a null packet for the given stream.
 */
int  packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index);

/**
 * Initialize queue resources and state.
 */
int  packet_queue_init(PacketQueue *q);

/**
 * Remove and free all queued packets.
 */
void packet_queue_flush(PacketQueue *q);

/**
 * Flush queue and release all queue resources.
 */
void packet_queue_destroy(PacketQueue *q);

/**
 * Abort queue operations and wake waiters.
 */
void packet_queue_abort(PacketQueue *q);

/**
 * Start queue operations after abort state.
 */
void packet_queue_start(PacketQueue *q);

/**
 * Dequeue one packet; block optionally.
 */
int  packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

/**
 * Return non-zero if queue is aborted.
 */
int  packet_queue_is_aborted(PacketQueue *q);

#ifdef __cplusplus
}
#endif

#endif // FFPLAY_GUI_PACKET_QUEUE_H
