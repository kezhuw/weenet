#include "pipe.h"
#include "atomic.h"
#include "memory.h"
#include "process.h"
#include "schedule.h"

#include <pthread.h>
#include <sys/uio.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

struct worker {
	struct {
		int64_t write;
	} lock;
	struct bpipe *pipe;
	pthread_t tid;
};

struct schedule {
	size_t nworker;
	struct worker workers[];
};

static struct schedule *S;

static void
_push(struct schedule *s, struct weenet_process *p) {
// Clang supports GCC’s pragma for compatibility with existing source code,
// as well as several extensions.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
	uint64_t hash;
	hash += (uint64_t)&hash + (uint64_t)p;
#pragma GCC diagnostic pop
	hash >>= 3;
	struct worker *w = s->workers + hash%s->nworker;

	struct iovec v[2];
	struct bpipe *pipe = w->pipe;
	weenet_atomic_lock(&w->lock.write);
	bpipe_writev(pipe, v);
	*((struct weenet_process **)v[0].iov_base) = p;
	bpipe_writen(pipe, sizeof(struct weenet_process *));
	weenet_atomic_unlock(&w->lock.write);
}

static void *
_work(void *arg) {
	struct bpipe *pipe = (struct bpipe *)arg;
	struct iovec v[2];
	for (;;) {
		size_t size = bpipe_readv(pipe, v);
		for (int i=0; i<2; ++i) {
			struct weenet_process **ptr = v[i].iov_base;
			struct weenet_process **end = (struct weenet_process **)((char*)ptr + v[i].iov_len);
			while (ptr < end) {
				struct weenet_process *p = *ptr++;
				if (weenet_process_resume(p)) {
					weenet_schedule_resume(p);
				}
				// Retained by resume.
				weenet_process_release(p);	
			}
		}
		bpipe_readn(pipe, size);
	}
	return NULL;
}

int
weenet_init_scheduler(size_t nthread) {
	assert(nthread != 0);
	assert(S == NULL);
	struct schedule *s = wcalloc(sizeof(*s) + sizeof(struct worker)*nthread);
	for (size_t i=0; i<nthread; ++i) {
		struct bpipe *pipe = bpipe_new(512, sizeof(struct weenet_process *));
		s->workers[i].pipe = pipe;
		s->workers[i].lock.write = 0;
		pthread_create(&s->workers[i].tid, NULL, _work, pipe);
	}
	s->nworker = nthread;
	S = s;
	return 0;
}

void
weenet_schedule_resume(struct weenet_process *p) {
	weenet_process_retain(p);
	_push(S, p);
}
