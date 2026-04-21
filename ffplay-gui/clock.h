#ifndef FFPLAY_GUI_CLOCK_H
#define FFPLAY_GUI_CLOCK_H

typedef struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    int paused;
    int (*queue_serial_getter)(void *opaque);
    void *queue_serial_opaque;
} Clock;

#ifdef __cplusplus
extern "C" {
#endif

double get_clock(Clock *c);
void set_clock_at(Clock *c, double pts, int serial, double time);
void set_clock(Clock *c, double pts, int serial);
void set_clock_speed(Clock *c, double speed);
void init_clock_with_serial_getter(Clock *c, int (*queue_serial_getter)(void *opaque), void *queue_serial_opaque);
void sync_clock_to_slave(Clock *c, Clock *slave);

#ifdef __cplusplus
}
#endif

#endif
