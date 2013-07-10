#define _POSIX_SOURCE	// for getnameinfo family (in c99 mode ?)
#define _BSD_SOURCE	// NI_MAXHOST/NI_MAXSERV need this under _POSIX_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <weenet.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netdb.h>

struct agent {
	int fd;
	process_t self;
	byte_t buf[1024];
	struct weenet_process *client;
};

static struct agent *
agent_new(struct weenet_process *p, uintptr_t data) {
	// XXX print remote address
	struct weenet_process *client = weenet_process_new("client", (uintptr_t)p, 0);
	if (client == NULL) {
		weenet_logger_fatalf("failed to start client.\n");
		return NULL;
	}

	process_t self = weenet_process_self(p);
	struct agent *g = wmalloc(sizeof(*g));
	int fd = (int)data;
	g->fd = fd;
	g->self = self;
	g->client = client;
	weenet_process_monitor(p, client);
	weenet_process_release(client);
	weenet_event_monitor(self, 0, fd, WEVENT_ADD, WEVENT_READ);

	struct sockaddr_storage saddr;
	socklen_t salen;
	if (getpeername(fd, (struct sockaddr *)&saddr, &salen) != 0) {
		perror("getpeername()");
		return g;
	}
	char addr[NI_MAXHOST], port[NI_MAXSERV];
	int err = getnameinfo((struct sockaddr *)&saddr, salen, addr, sizeof addr, port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV);
	if (err != 0) {
		fprintf(stderr, "getnameinfo() failed: %s\n", gai_strerror(err));
	}

	return g;
}

static void
agent_delete(struct agent *g) {
	close(g->fd);
	wfree(g);
}

static void
_send(struct agent *g, byte_t *buf, size_t len) {
	void *ptr = NULL;
	if (len != 0) {
		ptr = wmalloc(len);
		memcpy(ptr, buf, len);
	}
	weenet_process_push(g->client, g->self, 0, WMESSAGE_TYPE_CLIENT | WMESSAGE_RIDX_MEMORY, (uintptr_t)ptr, (uintptr_t)len); 
}

static void
_read_closed(struct agent *g) {
	weenet_event_monitor(g->self, 0, g->fd, WEVENT_DELETE, WEVENT_READ);
	weenet_process_push(g->client, g->self, 0, WMESSAGE_TYPE_CLIENT, 0, 0);
}

static int
agent_handle(struct agent *g, struct weenet_process *p, struct weenet_message *m) {
	int fd = g->fd;
	uint32_t type = weenet_message_type(m);
	switch (type) {
	case WMESSAGE_TYPE_RETIRED:
		g->client = NULL;
		weenet_process_retire(p);	// retire gate self
		break;
	case WMESSAGE_TYPE_EVENT:
		;uintreg_t event = (uintreg_t)m->meta;
		switch (event) {
		case WEVENT_READ:
			;size_t n = 0;
			for (;;) {
				ssize_t rd = read(fd, g->buf+n, sizeof(g->buf)-n);
				if (rd < 0) {
					int err = errno;
					switch (err) {
					case EAGAIN:
						if (n != 0) _send(g, g->buf, n);
						return 0;
					case EINTR:
						continue;
					default:
						weenet_logger_errorf("read(%d) failed(%s).\n", fd, strerror(err));
						if (n != 0) _send(g, g->buf, n);
						_send(g, NULL, 0);
						weenet_process_retire(p);
						return 0;
					}
				}
				n += rd;
				if (n == sizeof(g->buf)) {
					_send(g, g->buf, n);
					n = 0;
				}
				if (rd == 0) {
					_read_closed(g);
					break;
				}
			}
			break;
		default:
			break;
		}
		break;
	case WMESSAGE_TYPE_CLIENT:
		if (m->meta == 0) {
			weenet_process_retire(p);
		} else {
			// FIXME Write directly, if failed then monitor write event.
			write(fd, (void*)m->data, (size_t)m->meta);
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
