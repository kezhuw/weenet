#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <weenet.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>

#include <sys/socket.h>
#include <netdb.h>

struct agent {
	int fd;
	process_t self;
	struct weenet_process *client;
	struct {
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
		size_t maxsize;
		size_t cursize;
	} pending;
	byte_t buf[1024];
};

static struct agent *
agent_new(struct weenet_process *p, uintptr_t data) {
	// XXX print remote address
	struct weenet_process *client = weenet_process_new("client", (uintptr_t)p, 0);
	if (client == NULL) {
		weenet_logger_fatalf("failed to start client.");
		return NULL;
	}

	process_t self = weenet_process_self(p);
	struct agent *g = wcalloc(sizeof(*g));
	int fd = (int)data;
	g->fd = fd;
	g->self = self;
	g->client = client;
	g->pending.maxsize = 10240;
	weenet_process_monitor(p, client);
	weenet_process_release(client);
	weenet_event_monitor(self, 0, fd, WEVENT_ADD, WEVENT_WRITE);

	int bufsize = 10240;
	int err_ = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
	assert(err_ == 0);

	struct sockaddr_storage saddr;
	socklen_t salen = sizeof(saddr);
	if (getpeername(fd, (struct sockaddr *)&saddr, &salen) != 0) {
		perror("getpeername()");
		return g;
	}
	char addr[NI_MAXHOST], port[NI_MAXSERV];
	int err = getnameinfo((struct sockaddr *)&saddr, salen, addr, sizeof addr, port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV);
	if (err != 0) {
		fprintf(stderr, "getnameinfo() failed: %s\n", gai_strerror(err));
		return g;
	}

	weenet_logger_printf("new connection[%d] from %s:%s", fd, addr, port);

	return g;
}

static void
agent_delete(struct agent *g) {
	if (g->pending.head.msg != NULL) {
		weenet_message_unref(g->pending.head.msg);
		for (size_t i=0, n=g->pending.tail.num; i<n; ++i) {
			weenet_message_unref(g->pending.tail.slots[i]);
		}
	}
	if (g->pending.tail.slots != NULL) {
		wfree(g->pending.tail.slots);
	}
	weenet_event_monitor(g->self, 0, g->fd, WEVENT_DELETE, WEVENT_READ);
	weenet_event_monitor(g->self, 0, g->fd, WEVENT_DELETE, WEVENT_WRITE);
	close(g->fd);
	wfree(g);
}

static void
_send(struct agent *g, byte_t *buf, size_t len) {
	if (g->client == NULL) return;
	void *ptr = NULL;
	if (len != 0) {
		ptr = wmalloc(len);
		memcpy(ptr, buf, len);
	}
	weenet_process_push(g->client, g->self, 0, WMESSAGE_TYPE_CLIENT | WMESSAGE_RIDX_MEMORY, (uintptr_t)ptr, (uintptr_t)len); 
}

static size_t
_read(int fd, byte_t *buf, size_t len, bool *closed) {
	size_t n = 0;
	for (;;) {
		ssize_t rd = read(fd, buf, len);
		if (rd < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (rd == 0) {
			*closed = true;
			break;
		}
		n += (size_t)rd;
		assert((size_t)rd <= len);
		len -= (size_t)rd;
		if (len == 0) break;
		buf += rd;
	}
	return n;
}

static size_t
_write(int fd, byte_t *data, size_t size) {
	size_t n = 0;
	for (;;) {
		ssize_t wr = write(fd, (void*)data, size);
		if (wr < 0) {
			if (errno == EINTR) continue;
			break;
		}
		assert((size_t)wr <= size);
		n += (size_t)wr;
		size -= (size_t)wr;
		if (size == 0) break;
		data += wr;
	}
	return n;
}

static void
_read_closed(struct agent *g) {
	weenet_event_monitor(g->self, 0, g->fd, WEVENT_DELETE, WEVENT_READ);
	weenet_process_push(g->client, g->self, 0, WMESSAGE_TYPE_CLIENT, 0, 0);
}

static int
_event_read(struct agent *g, struct weenet_process *p) {
	int fd = g->fd;
	bool closed = false;
	for (;;) {
		size_t n = _read(fd, g->buf, sizeof(g->buf), &closed);
		if (n != 0) {
			_send(g, g->buf, n);
		}
		if (closed) {
			_read_closed(g);
			break;
		}
		if (n < sizeof(g->buf)) {
			int err = errno;
			if (err == EAGAIN) return 0;
			_send(g, NULL, 0);
			weenet_event_monitor(g->self, 0, fd, WEVENT_DELETE, WEVENT_READ);
			weenet_logger_errorf("read(%d) failed(%s).", fd, strerror(err));
			weenet_process_retire(p);
			return -1;
		}
	}
	return 0;
}

static int
_event_write(struct agent *g, struct weenet_process *p) {
	int fd = g->fd;
	if (g->pending.head.msg == NULL) {
		weenet_event_monitor(g->self, 0, fd, WEVENT_ADD, WEVENT_READ);
		return 0;
	}
	struct weenet_message *msg = g->pending.head.msg;
	byte_t *data = g->pending.head.data;
	size_t size = g->pending.head.size;
	size_t i = 0, n = g->pending.tail.num;
	for (;;) {
		size_t wr = _write(fd, (void*)data, size);
		if (wr < size) {
			g->pending.head.msg = msg;
			g->pending.head.data = data+wr;
			g->pending.head.size = size-wr;
			g->pending.tail.num = n-i;
			g->pending.cursize -= wr;
			if (i != n) {
				size_t j = 0;
				do {
					g->pending.tail.slots[j++] = g->pending.tail.slots[i++];
				} while (i<n);
				assert(j == g->pending.tail.num);
			}
			int err = errno;
			if (err == EAGAIN) {
				return 0;
			}
			weenet_event_monitor(g->self, 0, fd, WEVENT_DELETE, WEVENT_WRITE);
			weenet_logger_errorf("write(%d) failed(%s)", fd, strerror(err));
			weenet_process_retire(p);
			return -1;
		}
		g->pending.cursize -= size;
		weenet_message_unref(msg);
		if (i == n) {
			g->pending.head.msg = NULL;
			g->pending.tail.num = 0;
			weenet_event_monitor(g->self, 0, fd, WEVENT_ADD, WEVENT_READ);
			break;
		} else {
			msg = g->pending.tail.slots[i++];
			data = (byte_t*)msg->data;
			size = (size_t)msg->meta;
		}
	}
	return 0;
}

static int
_event_client(struct agent *g, struct weenet_process *p, struct weenet_message *m) {
	process_t self = g->self;
	int fd = g->fd;
	if (m->meta == 0) {
		weenet_process_retire(p);
	} else if (g->pending.head.msg != NULL) {
		size_t num = g->pending.tail.num;
		if (num == g->pending.tail.len) {
			size_t len = 2*num + 1;
			g->pending.tail.slots = wrealloc(g->pending.tail.slots, sizeof(struct weenet_message*)*len);
			g->pending.tail.len = len;
		}
		g->pending.tail.slots[num++] = weenet_message_ref(m);
		g->pending.tail.num = num;
		g->pending.cursize += (size_t)m->meta;
		if (g->pending.cursize >= g->pending.maxsize) {
			weenet_event_monitor(self, 0, fd, WEVENT_DELETE, WEVENT_READ);
		}
	} else {
		byte_t *data = (byte_t*)m->data;
		size_t size = (size_t)m->meta;
		size_t wr = _write(fd, data, size);
		if (wr < size) {
			int err = errno;
			if (err == EAGAIN) {
				size -= wr;
				g->pending.head.msg = weenet_message_ref(m);
				g->pending.head.data = data+wr;
				g->pending.head.size = size;
				g->pending.cursize = size;
				if (size >= g->pending.maxsize) {
					weenet_event_monitor(self, 0, fd, WEVENT_DELETE, WEVENT_READ);
				}
				return 0;
			}
			weenet_event_monitor(self, 0, fd, WEVENT_DELETE, WEVENT_WRITE);
			weenet_logger_errorf("write(%d) failed(%s)", fd, strerror(err));
			weenet_process_retire(p);
			return -1;
		}
	}
	return 0;
}

static int
agent_handle(struct agent *g, struct weenet_process *p, struct weenet_message *m) {
	uint32_t type = weenet_message_type(m);
	switch (type) {
	case WMESSAGE_TYPE_RETIRED:
		g->client = NULL;
		weenet_process_retire(p);	// retire gate self
		break;
	case WMESSAGE_TYPE_EVENT:
		if (g->client == NULL) break;
		uintreg_t event = (uintreg_t)m->meta;
		switch (event) {
		case WEVENT_READ:
			_event_read(g, p);
			break;
		case WEVENT_WRITE:
			_event_write(g, p);
			break;
		default:
			break;
		}
		break;
	case WMESSAGE_TYPE_CLIENT:
		_event_client(g, p, m);
		break;
	default:
		break;
	}

	return 0;
}

const struct weenet_interface agent_service = {
	.new		= (service_new_t)agent_new,
	.delete		= (service_delete_t)agent_delete,
	.handle		= (service_handle_t)agent_handle,
};
