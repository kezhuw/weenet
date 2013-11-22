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

#if defined(__linux__)
#include "linux/epoll.c"
#elif defined(__FreeBSD__) || defined(__APPLE__)
#include "freebsd/kqueue.c"
#endif
