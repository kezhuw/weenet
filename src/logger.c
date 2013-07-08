#include "atomic.h"
#include "logger.h"
#include "memory.h"
#include "process.h"

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#include <sys/stat.h>
#include <sys/types.h>

static struct weenet_process *L;

enum { BLOCK_SIZE = 1024*1024 };

struct chunk {
	uint32_t info;
	char bytes[];
};

#define ESIZE	(sizeof(struct chunk)+8)

static uint32_t
_pack(size_t size, bool busy) {
	return (uint32_t)(((uint32_t)busy << 31) | size);
}

#define _size(info)	((info) & 0x7FFFFFFF)

static void
_unpack(uint32_t info, size_t *size, bool *busy) {
	*size = (size_t)_size(info);
	*busy = (bool)(info >> 31);
}

struct block {
	struct block *link;
	char bytes[BLOCK_SIZE];
};

struct memory {
	int64_t lock;
	struct chunk *curr;
	struct {
		struct chunk *chk;
		struct block *blk;
	} fail;		// last failure allocation
	struct block *busy;
	struct block *last;
	struct block first;
};

static struct memory *M;

#define _lock(m)	weenet_atomic_lock(&m->lock)
#define _unlock(m)	weenet_atomic_unlock(&m->lock)

static struct memory *
_new() {
	struct memory *m = wmalloc(sizeof(struct memory));
	m->last = &m->first;
	m->busy = m->last;
	m->curr = (struct chunk *)m->first.bytes;
	m->curr->info = _pack(BLOCK_SIZE, 1);
	return m;
}

static void
_next(struct memory *m) {
	if (m->busy == m->last) {
		if (m->fail.chk != NULL) {
			m->curr = m->fail.chk;
			m->busy = m->fail.blk;
			m->fail.chk = NULL;
		} else {
			m->busy = &m->first;
			m->curr = (struct chunk *)m->first.bytes;
		}
	} else {
		m->busy = m->busy->link;
		m->curr = (struct chunk *)m->busy->bytes;
	}
}

#define aligned(s)	((s+3)&~3)

static void *
_block(struct memory *m, size_t size) {
	assert(m->fail.chk == NULL);
	m->fail.chk = m->curr;
	m->fail.blk = m->busy;
	struct block *b = wmalloc(sizeof *b);
	b->link = NULL;
	m->last->link = b;
	m->busy = m->last = b;
	struct chunk *c = (struct chunk *)b->bytes;
	c->info = _pack(size, true);
	m->curr = (struct chunk *)((char*)c + size);
	m->curr->info = _pack(BLOCK_SIZE-size, false);
	return c->bytes;
}

static void *
_malloc(struct memory *m, size_t size) {
	size = aligned(size) + sizeof(struct chunk);
	void *ptr = NULL;
	_lock(m);
	for (;;) {
		struct chunk *it = m->curr;
		size_t len;
		bool busy;
		_unpack(it->info, &len, &busy);
		if (busy) {
			ptr = _block(m, size);
			break;
		}

		char *end = m->busy->bytes + BLOCK_SIZE;
		struct chunk *next = (struct chunk *)((char*)it + len);
		while (end != (char*)next) {
			size_t n_size;
			bool n_used;
			_unpack(next->info, &n_size, &n_used);
			if (busy) break;
			len += n_size;
			next = (struct chunk *)((char*)it + len);
		}
		if (len < size) {
			it->info = _pack(len, false);
			if (end == (char*)next) {
				_next(m);
			} else {
				ptr = _block(m, size);
				break;
			}
		} else {
			if (len < size+ESIZE) {
				it->info = _pack(len, true);
				if (end == (char*)next) {
					_next(m);
				} else {
					m->curr = next;
				}
			} else {
				it->info = _pack(size, true);
				next = (struct chunk *)((char*)it + size);
				next->info = _pack(len-size, false);
				m->curr = next;
			}
			ptr = it->bytes;
			break;
		}
	}
	_unlock(m);
	assert(ptr != NULL);
	return ptr;
}

static void
_free(void *ptr) {
	struct chunk *c = (struct chunk *)((char*)ptr - offsetof(struct chunk, bytes));
	size_t size;
	bool busy;
	_unpack(c->info, &size, &busy);
	if (busy == false) {
		weenet_logger_fatalf("memory cruption in logger");
		return;
	}
	c->info = _pack(size, false);
	weenet_atomic_sync();
}

static void
_wrapped_free(void *ud, uintptr_t data, uintptr_t size) {
	(void)ud; (void)size;
	void *ptr = (void*)data;
	_free(ptr);
}

int
weenet_init_logger(const char *dir, size_t limit) {
	if (mkdir(dir, 0644) != 0 && errno == EACCES) {
		fprintf(stderr, "mkdir(%s, 0644) failed: %s\n", dir, strerror(EACCES));
		return -1;
	}
	assert(L == NULL);
	L = weenet_process_new("logger", (uintptr_t)dir, (uintptr_t)limit);
	if (L == NULL) return -1;
	M = _new();

	weenet_message_gc(WMESSAGE_RIDX_LOG, NULL, _wrapped_free);

	return 0;
}

void
_vprintf(const char *prefix, size_t len, const char *fmt, va_list args) {
	char buf[1024];
	int n = vsnprintf(buf, sizeof(buf), fmt, args);
	va_list saved_args;
	va_copy(saved_args, args);
	// XXX truncate large size
	size_t size = (size_t)n + len;
	char *ptr = _malloc(M, size);
	memcpy(ptr, prefix, len);
	if ((size_t)n > sizeof(buf)) {
		vsnprintf(ptr+len, n, fmt, saved_args);
	} else {
		memcpy(ptr+len, buf, n);
	}
	weenet_process_push(L, 0, 0, /**/0, (uintptr_t)ptr, (uintptr_t)size);
}

#define S_PRINT		""
#define S_ERROR		"ERROR "
#define S_FATAL		"FATAL "

void
weenet_logger_printf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_vprintf(S_PRINT, sizeof(S_PRINT)-1, fmt, args);
}

void
weenet_logger_errorf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_vprintf(S_ERROR, sizeof(S_ERROR)-1, fmt, args);
}

void
weenet_logger_fatalf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_vprintf(S_FATAL, sizeof(S_FATAL)-1, fmt, args);
}
