#ifndef __WEENET_TIMER_H_
#define __WEENET_TIMER_H_

#include "types.h"

#include <stdint.h>

void weenet_timer_boot();

uint64_t weenet_timer_now();
void weenet_timer_timeout(process_t pid, session_t sid, uint64_t msecs);

#endif
