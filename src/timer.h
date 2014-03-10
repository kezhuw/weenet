#ifndef __WEENET_TIMER_H_
#define __WEENET_TIMER_H_

#include "types.h"

#include <stdint.h>

// Millisecond resolution.

// zero-based monotonic time from startup.
uint64_t weenet_time();

// realtime when weenet startup.
uint64_t weenet_starttime();

// monotonic realtime, same as weenet_starttime() + weenet_time().
uint64_t weenet_realtime();


void weenet_timeout(process_t pid, session_t session, uint64_t msecs);

// update monotonic time, return updated weenet_time.
uint64_t weenet_update_time();

int weenet_init_time();

#endif
