#include "clock.h"

#include <math.h>

#include "packet_queue.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"

#define CLOCK_NOSYNC_THRESHOLD 10.0

struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int serial;
    int paused;
    int (*queue_serial_getter)(void *opaque);
    void *queue_serial_opaque;
};

static int packet_queue_serial_getter(void *opaque)
{
    return packet_queue_get_serial((PacketQueue *)opaque);
}

static int clock_serial_getter(void *opaque)
{
    return clock_get_serial((Clock *)opaque);
}

Clock *clock_create(void)
{
    return av_mallocz(sizeof(Clock));
}

void clock_destroy(Clock **c)
{
    if (!c || !*c)
        return;
    av_freep(c);
}

double get_clock(Clock *c)
{
    int queue_serial = c->queue_serial_getter(c->queue_serial_opaque);
    if (queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

double clock_get_pts(const Clock *c)
{
    return c->pts;
}

double clock_get_last_updated(const Clock *c)
{
    return c->last_updated;
}

double clock_get_speed(const Clock *c)
{
    return c->speed;
}

int clock_get_serial(const Clock *c)
{
    return c->serial;
}

int clock_get_paused(const Clock *c)
{
    return c->paused;
}

void clock_set_paused(Clock *c, int paused)
{
    c->paused = paused;
}

void clock_init_from_packet_queue(Clock *c, PacketQueue *q)
{
    init_clock_with_serial_getter(c, packet_queue_serial_getter, q);
}

void clock_init_from_clock(Clock *c, Clock *serial_source)
{
    init_clock_with_serial_getter(c, clock_serial_getter, serial_source);
}

void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

void init_clock_with_serial_getter(Clock *c, int (*queue_serial_getter)(void *opaque), void *queue_serial_opaque)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial_getter = queue_serial_getter;
    c->queue_serial_opaque = queue_serial_opaque;
    set_clock(c, NAN, -1);
}

void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > CLOCK_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, clock_get_serial(slave));
}
