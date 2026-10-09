#ifndef FRAMEPROF_H
#define FRAMEPROF_H

#include <stdbool.h>

// Where a slow pass of the interface loop went.
//
// LVGL marks its own work -- refresh, layout, each kind of drawing, image
// decoders, timers, events -- with LV_PROFILER_* points, which lv_conf.h
// sends here. Over one pass of lv_timer_handler() the time inside each point
// is summed; a pass that took longer than FRAMEPROF_SLOW_MS is written to the
// log with the points that took most of it, as "tag self/total ms xcount" --
// self is the time in the point minus the points inside it -- and the timer
// callbacks that took longest, by address. Only on the interface thread, and
// only when <card>/.local/frameprof exists: each point reads the clock, which
// on the Blaze is a system call.
//
// Included by LVGL's own sources (LV_PROFILER_INCLUDE), so plain C only.

#define FRAMEPROF_SLOW_MS 40

// `threshold_ms` 0 keeps FRAMEPROF_SLOW_MS. The simulator, many times faster
// than the device, takes a lower one from SONIX_FRAMEPROF.
void frameprof_init(bool enabled, unsigned threshold_ms);
void frameprof_pass_begin(void);
void frameprof_pass_end(void);

void frameprof_begin(const char *tag);
void frameprof_end(const char *tag);

// For timerprof.c: whether timers are being timed now, the clock it uses, and
// one timer callback's run.
bool frameprof_timing(void);
unsigned long long frameprof_now_us(void);
void frameprof_timer_ran(void *cb, unsigned us);
void frameprof_event_ran(void *cb, unsigned us);

// Whether this run is being profiled at all, decided once: the simulator's
// variable, or the trigger file on the card. Asked from the very first event
// callback the interface registers, which is long before the main loop.
bool frameprof_wanted(void);

#endif /* FRAMEPROF_H */
