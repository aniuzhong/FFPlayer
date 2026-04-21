/*
 * Clock implementation.
 *
 * Add getters/setters:
 * - clock_get_pts
 * - clock_get_last_updated
 * - clock_get_speed
 * - clock_get_serial
 * - clock_get_paused
 * - clock_set_paused
 *
 * Keep queue serial source binding inside the clock module:
 * - clock_init_from_packet_queue
 * - clock_init_from_clock
 */

#ifndef FFPLAY_GUI_CLOCK_H
#define FFPLAY_GUI_CLOCK_H

typedef struct PacketQueue PacketQueue;
typedef struct Clock Clock;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate and initialize a clock instance.
 */
Clock *clock_create(void);

/**
 * Destroy and free a clock instance.
 */
void clock_destroy(Clock **c);

/**
 * Return effective clock value considering drift, speed, and pause state.
 */
double get_clock(Clock *c);

/**
 * Return raw pts stored in clock.
 */
double clock_get_pts(const Clock *c);

/**
 * Return last update timestamp in seconds.
 */
double clock_get_last_updated(const Clock *c);

/**
 * Return playback speed for this clock.
 */
double clock_get_speed(const Clock *c);

/**
 * Return current clock serial value.
 */
int clock_get_serial(const Clock *c);

/**
 * Return non-zero if clock is paused.
 */
int clock_get_paused(const Clock *c);

/**
 * Set paused state for this clock.
 */
void clock_set_paused(Clock *c, int paused);

/**
 * Initialize clock and bind serial source to a packet queue.
 */
void clock_init_from_packet_queue(Clock *c, PacketQueue *q);

/**
 * Initialize clock and bind serial source to another clock.
 */
void clock_init_from_clock(Clock *c, Clock *serial_source);

/**
 * Set clock at a specified wall-clock time.
 */
void set_clock_at(Clock *c, double pts, int serial, double time);

/**
 * Set clock using current wall-clock time.
 */
void set_clock(Clock *c, double pts, int serial);

/**
 * Update clock speed while preserving continuity.
 */
void set_clock_speed(Clock *c, double speed);

/**
 * Initialize clock with generic serial getter callback.
 */
void init_clock_with_serial_getter(Clock *c, int (*queue_serial_getter)(void *opaque), void *queue_serial_opaque);

/**
 * Pull clock to slave clock if drift exceeds threshold.
 */
void sync_clock_to_slave(Clock *c, Clock *slave);

#ifdef __cplusplus
}
#endif

#endif // FFPLAY_GUI_CLOCK_H
