#include "event.h"
#include "atomic.h"
#include "logger.h"
#include "memory.h"
#include "process.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <pthread.h>

inline static void
_send(process_t pid, session_t session, uintptr_t fd, uintptr_t event) {
	uint32_t kind = session == 0 ? 0 : WMSG_KIND_RESPONSE;
	uint32_t tags = weenet_combine_tags(0, kind, WMSG_CODE_EVENT);
	weenet_process_send(pid, 0, session, tags, fd, event);
}

#if defined(__linux__)
#include "linux/epoll.c"
#elif defined(__FreeBSD__) || defined(__APPLE__)
#include "freebsd/kqueue.c"
#endif
