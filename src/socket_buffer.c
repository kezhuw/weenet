#include "socket_buffer.h"

#include "event.h"
#include "utils.h"
#include "memory.h"
#include "process.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <unistd.h>

static size_t
_write(int fd, byte_t *data, size_t size, int *err) {
	*err = 0;
	size_t n = 0;
	for (;;) {
		ssize_t wr = write(fd, (void*)data, size);
		if (wr < 0) {
			if (errno == EINTR) continue;
			*err = errno;
			break;
		}
		size_t wz = (size_t)wr;
		n += wz;
		size -= wz;
		if (size == 0) break;
		data += wz;
	}
	return n;
}

struct socket_buffer {
	process_t self;
	int fd;
	size_t maxsize;
	size_t cursize;
	struct {
		struct weenet_message *msg;
		byte_t *data;
		size_t size;
	} head;
	struct {
		size_t num;
		size_t len;
		struct weenet_message **slots;
	} tail;
};

static void
_expend_on_demand(struct socket_buffer *b, size_t n) {
	size_t num = b->tail.num + n;
	if (num > b->tail.len) {
		b->tail.len += num;
		b->tail.slots = wrealloc(b->tail.slots, sizeof(struct weenet_message *) * b->tail.len);
	}
}

struct socket_buffer *
socket_buffer_new(process_t self, int fd, int maxsize) {
	struct socket_buffer *b = wcalloc(sizeof(*b));
	b->self = self;
	b->fd = fd;
	b->maxsize = (size_t)maxsize;
	weenet_event_monitor(self, 0, fd, WEVENT_DELETE, WEVENT_WRITE);
	return b;
}

void
socket_buffer_delete(struct socket_buffer *b) {
	if (b->head.msg != NULL) {
		weenet_message_unref(b->head.msg);
		for (size_t i=0, n=b->tail.num; i<n; ++i) {
			weenet_message_unref(b->tail.slots[i]);
		}
	}
	if (b->tail.slots != NULL) {
		wfree(b->tail.slots);
	}
	weenet_event_monitor(b->self, 0, b->fd, WEVENT_DELETE, WEVENT_READ);
	weenet_event_monitor(b->self, 0, b->fd, WEVENT_DELETE, WEVENT_WRITE);
}

int
socket_buffer_trash(struct socket_buffer *b, process_t source) {
	if (b->head.msg == NULL) {
		return 0;
	}

	int nTrashed = 0;

	if (b->head.msg->source == source && (size_t)b->head.msg->meta == b->head.size) {
		weenet_message_unref(b->head.msg);
		b->head.msg = NULL;
		b->cursize -= b->head.size;
		++nTrashed;
	}

	size_t j = 0;
	size_t accsize = 0;
	for (size_t i=0, n=b->tail.num; i<n; ++i) {
		struct weenet_message *m = b->tail.slots[i];
		if (m->source == source) {
			b->tail.slots[i] = NULL;
			accsize += (size_t)m->meta;
			weenet_message_unref(m);
			++nTrashed;
			continue;
		}
		if (b->head.msg == NULL) {
			b->tail.slots[i] = NULL;
			b->head.msg = m;
			b->head.data = (byte_t*)m->data;
			b->head.size = (size_t)m->meta;
			continue;
		}
		// ok for i == j.
		b->tail.slots[j++] = m;
	}
	b->tail.num = j;
	b->cursize -= accsize;

	if (b->head.msg == NULL) {
		b->tail.num = 0;
		assert(b->cursize == 0);
		weenet_event_monitor(b->self, 0, b->fd, WEVENT_ADD, WEVENT_READ);
	}
	return nTrashed;
}

int
socket_buffer_event(struct socket_buffer *b) {
	int fd = b->fd;
	if (b->head.msg == NULL) {
		weenet_event_monitor(b->self, 0, fd, WEVENT_ADD, WEVENT_READ);
		return 0;
	}
	struct weenet_message *m = b->head.msg;
	byte_t *data = b->head.data;
	size_t size = b->head.size;
	size_t i = 0, n = b->tail.num;
	size_t accsize = 0;
	for (;;) {
		int err;
		size_t wr = _write(fd, data, size, &err);
		if (wr < size) {
			b->head.msg = m;
			b->head.data = data+wr;
			b->head.size = size-wr;
			b->tail.num = n-i;
			b->cursize -= wr + accsize;
			if (i != n && i != 0) {
				size_t j = 0;
				do {
					b->tail.slots[j++] = b->tail.slots[i++];
				} while (i<n);
				assert(j == b->tail.num);
			}
			if (err == EAGAIN) {
				return 0;
			}
			weenet_event_monitor(b->self, 0, fd, WEVENT_DELETE, WEVENT_WRITE);
			return err;
		}
		accsize += size;
		weenet_message_unref(m);

		if (i == n) {
			b->head.msg = NULL;
			b->tail.num = 0;
			b->cursize -= accsize;
			assert(b->cursize == 0);
			weenet_event_monitor(b->self, 0, fd, WEVENT_ADD, WEVENT_READ);
			break;
		}

		m = b->tail.slots[i++];
		data = (byte_t*)m->data;
		size = (size_t)m->meta;
	}
	return 0;
}

int
socket_buffer_write(struct socket_buffer *b, struct weenet_message *m) {
	if (b->head.msg != NULL) {
		_expend_on_demand(b, 1);
		b->tail.slots[b->tail.num++] = weenet_message_ref(m);
		b->cursize += (size_t)m->meta;
		if (b->cursize >= b->maxsize) {
			weenet_event_monitor(b->self, 0, b->fd, WEVENT_DELETE, WEVENT_READ);
		}
	} else {
		byte_t *data = (byte_t*)m->data;
		size_t size = (size_t)m->meta;
		int err;
		size_t wr = _write(b->fd, data, size, &err);
		if (wr < size) {
			if (err == EAGAIN) {
				b->head.msg = weenet_message_ref(m);
				b->head.data = data+wr;
				b->cursize = b->head.size = size-wr;
				if (b->cursize >= b->maxsize) {
					weenet_event_monitor(b->self, 0, b->fd, WEVENT_DELETE, WEVENT_READ);
				}
				return 0;
			}
			weenet_event_monitor(b->self, 0, b->fd, WEVENT_DELETE, WEVENT_WRITE);
			return err;
		}
	}
	return 0;
}
