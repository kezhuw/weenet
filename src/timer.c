#include "atomic.h"
#include "memory.h"
#include "process.h"
#include "slab.h"
#include "timer.h"
#include "utils.h"

#include <sys/time.h>

#include <assert.h>

// XXX Cancelable timer ?
// struct timer_ref {
//	// ...
//	bool cancel;
// };
//
// Store in 'data' field of weenet_message with resource id 'WMESSAGE_RIDX_TIMER' ?
// Need helpness from message dispatcher.

#define FIRST_NODE_FIELD	struct node *link

struct node {
	FIRST_NODE_FIELD;
	process_t source;
	session_t session;
	uint64_t expired;
};

struct list {
	struct _unused {
		FIRST_NODE_FIELD;
	} dummy;
	struct node *tail;
};

inline static void
_list_init(struct list *l) {
	l->tail = (struct node *)&l->dummy;
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

// 64M memory, 70 Minutes (64-bits machine)
#define TIME_SHIFT	20
#define TIME_SCALE	(1 << TIME_SHIFT)
#define TIME_TOTAL	(TIME_SCALE<<2)
#define TIME_SMASK	(TIME_SCALE-1)
#define TIME_TMASK	(TIME_TOTAL-1)

struct time {
	uint64_t now;
	uint64_t start;

	struct slab *node_slab;

	uint64_t lock;	// protect 'time', 'extra', 'wheels'
	uint64_t time;
	struct list extra;
	struct list wheels[TIME_TOTAL];
};

static uint64_t _now();

static void
_init(struct time *t) {
	t->start = t->now = _now();
	t->node_slab = slab_new(5000, sizeof(struct node));
	t->lock = 0;
	t->time = 1;	// or t->now ?
	_list_init(&t->extra);
	for (int i=0, n=nelem(t->wheels); i<n; ++i) {
		_list_init(&t->wheels[i]);
	}
}

#define _lock(t)	weenet_atomic_lock(&(t)->lock)
#define _unlock(t)	weenet_atomic_unlock(&(t)->lock)

inline static struct node *
_new_node(struct time *t, process_t pid, session_t session) {
	struct node *n = slab_retain(t->node_slab);
	n->source = pid;
	n->session = session;
	return n;
}

inline static void
_free_node(struct time *t, struct node *n) {
	slab_release(t->node_slab, n);
}

inline static void
_send(process_t pid, session_t sid) {
	weenet_process_send(pid, 0, sid, WMESSAGE_TYPE_TIMEO | WMESSAGE_FLAG_RESPONSE, 0, 0);
}

static void
_queue(struct time *t, process_t source, session_t session, uint64_t msecs) {
	struct node *n = _new_node(t, source, session);
	_lock(t);
	n->expired = t->time + msecs;
	if (msecs < TIME_TOTAL) {
		uint64_t index = n->expired & TIME_TMASK;
		_list_append(&t->wheels[index], n);
	} else {
		_list_append(&t->extra, n);
	}
	_unlock(t);
}

static void
_update(struct time *t) {
	struct node *pending = NULL;
	_lock(t);
	uint64_t time = t->time++;
	uint64_t index = time & TIME_TMASK;
	struct node *seqs = _list_clear(&t->wheels[index]);
	if ((time & TIME_SMASK) == 0) {
		pending = _list_clear(&t->extra);
	}
	_unlock(t);
	while (seqs != NULL) {
		struct node *n = seqs;
		seqs = n->link;
		_send(n->source, n->session);
		_free_node(t, n);
	}
	++time;
	while (pending != NULL) {
		struct node *n = pending;
		pending = n->link;
		if (n->expired <= time) {
			// XXX can't happen ?
			_send(n->source, n->session);
			_free_node(t, n);
		} else {
			uint64_t msecs = n->expired - time;
			_lock(t);
			if (msecs < TIME_TOTAL) {
				uint64_t index = n->expired & TIME_TMASK;
				_list_append(&t->wheels[index], n);
			} else {
				_list_append(&t->extra, n);
			}
			_unlock(t);
		}
	}
}

static uint64_t
_now() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64_t now = (uint64_t)tv.tv_sec*1000 + (uint64_t)tv.tv_usec/1000;
	return now;
}

static struct time *T;

uint64_t
weenet_time_now() {
	return T->now;
}

uint64_t
weenet_time_starttime() {
	return T->start;
}

void
weenet_time_update() {
	uint64_t now = _now();
	struct time *t = T;
	if (now > t->now) {
		uint64_t n = now - t->now;
		t->now = now;
		while (n--) {
			_update(t);
		}
	}
}

void
weenet_time_timeout(process_t source, session_t session, uint64_t msecs) {
	if (msecs == 0) {
		_send(source, session);
	} else {
		_queue(T, source, session, msecs);
	}
}

int
weenet_init_time() {
	assert(T == NULL);
	if (T != NULL) return -1;

	struct time *t = wmalloc(sizeof(*t));
	_init(t);
	T = t;
	return 0;
}
