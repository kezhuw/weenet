#ifndef __WEENET_EVENT_H_
#define __WEENET_EVENT_H_

#include "types.h"

enum wevent_op {
	WEVENT_ADD	= 1,
	WEVENT_ENABLE	= 2,		// EPOLL_CTL_MOD in linux
	WEVENT_DELETE	= 3,
};


#define WEVENT_MASK	0x00FF

enum wevent_filter {
	WEVENT_READ	= 1,
	WEVENT_WRITE	= 2,
	WEVENT_ERROR	= 3,		// Only return by event monitor
	WEVENT_EOF	= 4,		// must can be bitwised with READ/WRITE

	// WEVENT_CLEAR	= 0x1000,	// EPOLLET in linux, must be enfored in asynchronous reporting
	WEVENT_ONESHOT	= 0x2000,	// deleted after reported
	WEVENT_DISPATCH	= 0x4000,	// disabled after reported, EPOLLONESHOT
};

int weenet_event_start(int max);

// XXX Session shouldn't be long-time lived!
//
// Similar to kqueue/kevent, (fd, event) is a pair.
int weenet_event_monitor(process_t source, session_t session, int fd, int op, int event);

#endif
