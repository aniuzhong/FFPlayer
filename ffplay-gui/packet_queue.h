/*
 * Packet queue implementation.
 *
 * Add getters:
 * - packet_queue_get_nb_packets
 * - packet_queue_get_duration
 * - packet_queue_is_initialized
 * - packet_queue_get_serial
 * - packet_queue_is_aborted
 *
 * The goal is to achieve a migration equivalent to the behavior of ffplay.c,
 * and using a thin wrapper without locking for the getter is closer to the original intent.
 *
 * From the perspective of C's data race semantics, such lock-free shared reads are not rigorous.
 * However, in practical scenarios like ffplay, this approach has been adopted for a long time.
 */

#ifndef FFPLAY_GUI_PACKET_QUEUE_H
#define FFPLAY_GUI_PACKET_QUEUE_H

#include <SDL.h>
#include <SDL_thread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AVPacket AVPacket;

typedef struct PacketQueue PacketQueue;

/**
 * Allocate and initialize a packet queue.
 */
PacketQueue *packet_queue_create(void);

/**
 * Destroy and free a packet queue instance.
 */
void packet_queue_free(PacketQueue **q);

/**
 * Enqueue one packet into the queue (with locking).
 */
int packet_queue_put(PacketQueue *q, AVPacket *pkt);

/**
 * Enqueue a null packet for the given stream (with locking).
 */
int packet_queue_put_nullpacket(PacketQueue *q, AVPacket *pkt, int stream_index);

/**
 * Initialize queue resources and state (with locking).
 */
int  packet_queue_init(PacketQueue *q);

/**
 * Remove and free all queued packets (with locking).
 */
void packet_queue_flush(PacketQueue *q);

/**
 * Flush queue and release all queue resources (with locking).
 */
void packet_queue_destroy(PacketQueue *q);

/**
 * Abort queue operations and wake waiters (with locking).
 */
void packet_queue_abort(PacketQueue *q);

/**
 * Start queue operations after abort state (with locking).
 */
void packet_queue_start(PacketQueue *q);

/**
 * Dequeue one packet; block optionally (with locking).
 */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial);

/**
 * Return non-zero if queue is aborted (no locking).
 */
int packet_queue_is_aborted(PacketQueue *q);

/**
 * Return current queue serial value (no locking).
 */
int packet_queue_get_serial(PacketQueue *q);

/**
 * Return number of queued packets (no locking).
 */
int packet_queue_get_nb_packets(PacketQueue *q);

/**
 * Return total queued duration (no locking).
 */
int64_t packet_queue_get_duration(PacketQueue *q);

/**
 * Return total queued size in bytes (no locking).
 */
int packet_queue_get_size(PacketQueue *q);

/**
 * Return non-zero if queue internals are initialized (no locking).
 */
int packet_queue_is_initialized(PacketQueue *q);

#ifdef __cplusplus
}
#endif

#endif // FFPLAY_GUI_PACKET_QUEUE_H
