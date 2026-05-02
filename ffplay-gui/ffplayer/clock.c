#include <math.h>

#include <libavutil/common.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#include "clock.h"

#define CLOCK_NOSYNC_THRESHOLD 10.0

struct Clock {
    double pts;
    double pts_drift;
    double last_updated;
    double speed;
    int    serial;
    int    paused;
};

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

Clock *clock_create(void)
{
    Clock *c;

    c = av_mallocz(sizeof(Clock));
    if (!c)
        return NULL;
    c->speed = 1.0;
    set_clock(c, NAN, -1);
    return c;
}

void clock_destroy(Clock **c)
{
    if (!c || !*c)
        return;
    av_freep(c);
}

double get_clock(Clock *c, int authoritative_serial)
{
    if (authoritative_serial != c->serial)
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

void set_clock_speed(Clock *c, double speed, int authoritative_serial)
{
    set_clock(c, get_clock(c, authoritative_serial), c->serial);
    c->speed = speed;
}

void sync_clock_to_slave(Clock *c, int c_authoritative_serial,
                        Clock *slave, int slave_authoritative_serial)
{
    double clock = get_clock(c, c_authoritative_serial);
    double slave_clock = get_clock(slave, slave_authoritative_serial);

    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > CLOCK_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, clock_get_serial(slave));
}
