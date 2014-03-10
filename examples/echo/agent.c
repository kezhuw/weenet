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
	struct socket_buffer *socket_buffer;
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

	process_t self = weenet_process_pid(p);
	struct agent *g = wcalloc(sizeof(*g));
	int fd = (int)data;
	g->fd = fd;
	g->self = self;
	g->client = client;
	g->socket_buffer = socket_buffer_new(self, fd, 10240);
	weenet_process_monitor(client);
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
	socket_buffer_delete(g->socket_buffer);
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

#undef EEOF
#define EEOF	(-1)

static size_t
_read(int fd, byte_t *buf, size_t len, int *err) {
	*err = 0;
	size_t n = 0;
	for (;;) {
		ssize_t rd = read(fd, buf, len);
		if (rd == 0) {
			*err = EEOF;
			break;
		}
		if (rd < 0) {
			if (errno == EINTR) continue;
			*err = errno;
			break;
		}
		size_t rz = (size_t)rd;
		n += rz;
		len -= rz;
		if (len == 0) break;
		buf += rz;
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
	for (;;) {
		int err;
		size_t n = _read(fd, g->buf, sizeof(g->buf), &err);
		if (n != 0) {
			_send(g, g->buf, n);
		}
		switch (err) {
		case 0:
			break;
		case EEOF:
			_read_closed(g);
			// fall through
		case EAGAIN:
			return 0;
		default:
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
			;int err = socket_buffer_event(g->socket_buffer);
			if (err != 0) {
				weenet_logger_errorf("write(%d) failed(%s)", g->fd, strerror(err));
				weenet_process_retire(p);
			}
			break;
		default:
			break;
		}
		break;
	case WMESSAGE_TYPE_CLIENT:
		;int err = socket_buffer_write(g->socket_buffer, m);
		if (err != 0) {
			weenet_logger_errorf("write(%d) failed(%s)", g->fd, strerror(err));
			weenet_process_retire(p);
			return -1;
		}
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
