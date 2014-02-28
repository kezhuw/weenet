#include "atomic.h"
#include "memory.h"
#include "process.h"
#include "service.h"
#include "slab.h"
#include "timer.h"
#include "utils.h"

#include <sys/time.h>

#include <assert.h>
#include <limits.h>

// About algorithm See:
//
// http://blog.codingnow.com/2007/05/timer.html
// https://github.com/cloudwu/skynet/blob/6ba28cd4420a4bbc4c65a009efdb2bd02741e7c1/skynet-src/skynet_timer.c

// XXX Cancelable timer ?
// struct timer_ref {
//	// ...
//	bool cancel;
// };
//
// Store in 'data' field of weenet_message with resource id 'WMESSAGE_RIDX_TIMER' ?
// Need helpness from message dispatcher.
//
// OR
//
// keep some kind of timerRef(eg. (pid, session) pair) in source process's
// dictionary, before dispatch fired timer to user, check it.

#define FIRST_NODE_FIELD	struct node *link

struct node {
	FIRST_NODE_FIELD;
	uint64_t expire;
	// user data
	process_t source;
	session_t session;
};

struct list {
	struct _unused {
		FIRST_NODE_FIELD;
	} dummy;
	struct node *tail;
};

#define TIME_LEAST_SHIFT	14
#define TIME_LEAST_VALUE	(1<<TIME_LEAST_SHIFT)
#define TIME_LEAST_MASK		(TIME_LEAST_VALUE-1)

#define TIME_LEVEL_SHIFT	10
#define TIME_LEVEL_VALUE	(1<<TIME_LEVEL_SHIFT)
#define TIME_LEVEL_MASK		(TIME_LEVEL_VALUE-1)

#define TIME_BITS		(CHAR_BIT*(int)sizeof(uint64_t))
#define TIME_LEVEL_COUNT	((TIME_BITS-TIME_LEAST_SHIFT)/TIME_LEVEL_SHIFT)

static_assert(TIME_BITS == TIME_LEVEL_COUNT*TIME_LEVEL_SHIFT + TIME_LEAST_SHIFT, "time bits mismatch");

struct timer {
	uint64_t time;
	struct slab *node_slab;
	struct list least[TIME_LEAST_VALUE];
	struct list level[TIME_LEVEL_COUNT][TIME_LEVEL_VALUE-1];
};

inline static void
_list_init(struct list *l) {
	l->dummy.link = NULL;
	l->tail = (struct node *)&l->dummy;
}

inline static bool
_list_empty(struct list *l) {
	return l->dummy.link == NULL;
}

inline static void
_list_append(struct list *l, struct node *n) {
	l->tail->link = n;
	l->tail = n;
}

inline static struct node *
_list_clear(struct list *l) {
	l->tail->link = NULL;
	struct node *nodes = l->dummy.link;
	_list_init(l);
	return nodes;
}

inline static struct node *
_new_node(struct timer *t) {
	return slab_retain(t->node_slab);
}

inline static void
_free_node(struct timer *t, struct node *n) {
	slab_release(t->node_slab, n);
}

inline static void
_send(process_t pid, session_t sid) {
	weenet_process_send(pid, 0, sid, WMESSAGE_TYPE_TIMEO | WMESSAGE_FLAG_RESPONSE, 0, 0);
}

static uint64_t
_gettime() {
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (uint64_t)tp.tv_sec * 1000 + (uint64_t)tp.tv_nsec/1000000;
}

static uint64_t
_realtime() {
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	return (uint64_t)tp.tv_sec * 1000 + (uint64_t)tp.tv_nsec/1000000;
}

static void
_queue(struct timer *t, struct node *node) {
	uint64_t time = t->time;
	uint64_t expire = node->expire;
	assert(expire >= time);
	if (expire - time < TIME_LEAST_VALUE) {
		uint64_t index = expire & TIME_LEAST_MASK;
		_list_append(&t->least[index], node);
	} else {
		uint64_t level = 0;
		uint64_t exp2 = 1 << TIME_LEAST_SHIFT;
		do {
			exp2 <<= TIME_LEVEL_SHIFT;
			uint64_t mask = exp2 - 1;
			if ((expire | mask) == (time | mask)) {
				uint64_t shift = TIME_LEAST_SHIFT + level*TIME_LEVEL_SHIFT;
				uint64_t value = (expire >> shift) & TIME_LEVEL_MASK;
				_list_append(&t->level[level][value-1], node);
				break;
			}
		} while (++level < TIME_LEVEL_COUNT);
		assert(level < TIME_LEVEL_COUNT);
	}
}

static void
_tick(struct timer *t) {
	uint64_t index = t->time & TIME_LEAST_MASK;

	if (!_list_empty(&t->least[index])) {
		struct node *list = _list_clear(&t->least[index]);
		do {
			struct node *node = list;
			list = list->link;

			_send(node->source, node->session);
			_free_node(t, node);
		} while (list != NULL);
	}

	uint64_t time = ++t->time;
	if ((time & TIME_LEAST_MASK) == 0) {
		assert(time != 0);
		time >>= TIME_LEAST_SHIFT;
		uint64_t level=0;
		do {
			uint64_t value = time & TIME_LEVEL_MASK;
			if (value != 0) {
				struct node *list = _list_clear(&t->level[level][value-1]);
				while (list != NULL) {
					struct node *node = list;
					list = list->link;
					_queue(t, node);
				}
				break;
			}
			time >>= TIME_LEVEL_SHIFT;
		} while (++level < TIME_LEVEL_COUNT);
		assert(level < TIME_LEVEL_COUNT);
	}
}

static void
_update(struct timer *t, uint64_t time) {
	if (time > t->time) {
		for (uint64_t i=0, n = time - t->time; i<n; ++i) {
			_tick(t);
		}
	}
}

static void
_timeout(struct timer *t, uint64_t timeout, process_t source, session_t session) {
	struct node *node = _new_node(t);
	node->expire = t->time + timeout;
	node->source = source;
	node->session = session;
	_queue(t, node);
}

static struct timer *
_new() {
	struct timer *t = wmalloc(sizeof(*t));
	t->time = 0;

	for (int i=0; i<TIME_LEAST_VALUE; ++i) {
		_list_init(&t->least[i]);
	}
	for (int i=0; i<TIME_LEVEL_COUNT; ++i) {
		for (int j=0; j<TIME_LEVEL_VALUE-1; ++j) {
			_list_init(&t->level[i][j]);
		}
	}

	t->node_slab = slab_new(5000, sizeof(struct node));

	return t;
}

enum {
	TAGS_UPDATE_TIME	= (uint32_t)WMESSAGE_TYPE_UDEF_START,
	TAGS_REQUEST_TIMEOUT	= (uint32_t)WMESSAGE_TYPE_UDEF_STOP,
};

static int
_handle(struct timer *t, struct weenet_process *p, struct weenet_message *m) {
	(void)p;
	uint64_t time = (uint64_t)m->data;
	switch (m->tags) {
	case TAGS_UPDATE_TIME:
		_update(t, time);
		break;
	case TAGS_REQUEST_TIMEOUT:
		_timeout(t, time, m->source, m->session);
		break;
	default:
		break;
	}
	return 0;
}

static void
_delete(struct timer *t, struct weenet_process *p) {
	(void)p;
	slab_delete(t->node_slab);
	wfree(t);
}

static const struct weenet_interface timer_service = {
	.new		= (service_new_t)_new,
	.handle		= (service_handle_t)_handle,
	.delete		= (service_delete_t)_delete,
};


static uint64_t TIME;

// readonly after set.
static uint64_t STARTTIME;
static uint64_t STARTTIME_REALTIME;
static struct weenet_process *TIMER_SERVICE;

uint64_t
weenet_time() {
	return TIME;
}

uint64_t
weenet_starttime() {
	return STARTTIME_REALTIME;
}

uint64_t
weenet_realtime() {
	return weenet_starttime() + weenet_time();
}

void
weenet_timeout(process_t source, session_t session, uint64_t timeout) {
	weenet_process_push(TIMER_SERVICE, source, session, TAGS_REQUEST_TIMEOUT, (uintptr_t)timeout, 0);
}

uint64_t
weenet_update_time() {
	uint64_t now = _gettime();
	assert(now >= STARTTIME);
	uint64_t time = now - STARTTIME;
	if (time > TIME) {
		TIME = time;
		weenet_process_push(TIMER_SERVICE, 0, 0, TAGS_UPDATE_TIME, (uintptr_t)time, 0);
	}
	return TIME;
}

static const char *NAME = "timer";

int
weenet_init_time() {
	assert(TIMER_SERVICE == NULL);
	weenet_library_inject(weenet_atom_new(NAME, strlen(NAME)), &timer_service);
	STARTTIME = _gettime();
	STARTTIME_REALTIME = _realtime();
	TIMER_SERVICE = weenet_process_new(NAME, 0, 0);
	if (TIMER_SERVICE == NULL) {
		return -1;
	}
	return 0;
}

