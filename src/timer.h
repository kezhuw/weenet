#ifndef __WEENET_TIMER_H_
#define __WEENET_TIMER_H_

#include "types.h"

#include <stdint.h>

// Millisecond resolution.


uint64_t weenet_time_now();

// Return timestamp when weenet_init_time() called.
uint64_t weenet_time_starttime();

void weenet_time_timeout(process_t pid, session_t sid, uint64_t msecs);


int weenet_init_time();

// Called by time update thread.
void weenet_time_update();

#endif
