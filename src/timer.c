#include "timer.h"
#include "process.h"

void
weenet_timer_timeout(process_t pid, session_t sid, uint64_t msecs) {
	(void)msecs;
	// FIXME
	weenet_process_send(pid, 0, sid, WMESSAGE_TYPE_TIMEO, 0, 0);
}
