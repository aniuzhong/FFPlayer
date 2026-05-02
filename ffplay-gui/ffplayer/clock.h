/*
 * Clock implementation.
 *
 * Getters/setters:
 * - clock_get_pts
 * - clock_get_last_updated
 * - clock_get_speed
 * - clock_get_serial
 * - clock_get_paused
 * - clock_set_paused
 *
 * Obsolete-clock detection compares Clock.serial with authoritative_serial
 * passed into get_clock (callers supply queue serial via packet_queue_get_serial
 * or extclk identity via clock_get_serial(extclk)).
 */

#ifndef FFPLAY_GUI_CLOCK_H
#define FFPLAY_GUI_CLOCK_H

typedef struct Clock Clock;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate and initialize a clock instance (speed 1.0, not paused).
 */
Clock *clock_create(void);

/**
 * Destroy and free a clock instance.
 */
void clock_destroy(Clock **c);

/**
 * Return effective clock value considering drift, speed, and pause state.
 * authoritative_serial must match Clock.serial or the clock is stale (NAN).
 */
double get_clock(Clock *c, int authoritative_serial);

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
 * Set clock at a specified wall-clock time.
 */
void set_clock_at(Clock *c, double pts, int serial, double time);

/**
 * Set clock using current wall-clock time.
 */
void set_clock(Clock *c, double pts, int serial);

/**
 * Update clock speed while preserving continuity.
 * authoritative_serial is used for stale detection when sampling current PTS.
 */
void set_clock_speed(Clock *c, double speed, int authoritative_serial);

/**
 * Pull clock to slave clock if drift exceeds threshold.
 */
void sync_clock_to_slave(Clock *c, int c_authoritative_serial,
                         Clock *slave, int slave_authoritative_serial);

#ifdef __cplusplus
}
#endif

#endif // FFPLAY_GUI_CLOCK_H
