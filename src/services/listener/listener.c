#define _GNU_SOURCE	// for memrchr

#include "event.h"
#include "logger.h"
#include "memory.h"
#include "process.h"
#include "service.h"
#include "utils.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct listener {
	int fd;
	process_t self;
	monitor_t monitor;
	struct weenet_process *forward;
	char address[];
};

#define TCP_PREFIX	"tcp://"
#define TCP4_PREFIX	"tcp4://"
#define TCP6_PREFIX	"tcp6://"

// ":6666"
// "*:6060"
// "tcp://*:6666"
// "tcp6://:6666"
// "tcp4://12.34.0.39:6666"
static size_t
_trim(char *addr, int *family) {
	*family = AF_UNSPEC;
	if (strncmp(addr, TCP_PREFIX, sizeof(TCP_PREFIX)-1) == 0) {
		return sizeof(TCP_PREFIX)-1;
	} else if (strncmp(addr, TCP4_PREFIX, sizeof(TCP4_PREFIX)-1) == 0) {
		*family = AF_INET;
		return sizeof(TCP4_PREFIX)-1;
	} else if (strncmp(addr, TCP6_PREFIX, sizeof(TCP6_PREFIX)-1) == 0) {
		*family = AF_INET6;
		return sizeof(TCP6_PREFIX)-1;
	}
	return 0;
}

static int
_parse(char *addr, size_t len, int *family, char **host, char **port) {
	size_t n = _trim(addr, family);
	addr += n;
	len -= n;

	char *sep = memrchr(addr, ':', len);
	if (sep == NULL) return -1;

	char *beg = NULL;
	char *end = NULL;
	if (addr[0] == '[') {
		end = sep-1;
		if (*end != ']') {
			return -1;
		}
		beg = addr+1;
	} else {
		beg = addr;
		end = sep;
	}
	assert(beg <= end);
	if ((beg == end) || ((beg+1 == end) && beg[0] == '*')) {
		*host = NULL;
	} else {
		*end = '\0';
		*host = beg;
	}
	*port = sep+1;
	return 0;
}

static int
_noblocking(int fd) {
	int nb = 1;
	return ioctl(fd, FIONBIO, &nb);
}


static int
_option(int fd) {
	if (_noblocking(fd) == -1) return -1;
	int reuse = 1;
	return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

static int
_listen(const char *address, size_t len, int backlog) {
	char tmp[len+1];
	memcpy(tmp, address, len+1);
	address = tmp;

	char *host = NULL;
	char *port = NULL;
	int family = AF_UNSPEC;
	int err = _parse(tmp, len, &family, &host, &port);
	if (err != 0) {
		return -1;
	}

	struct addrinfo hint;
	memzero(&hint, sizeof hint);
	hint.ai_family = family;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_protocol = IPPROTO_TCP;
	hint.ai_flags = AI_PASSIVE;

	struct addrinfo *res;
	err = getaddrinfo(host, port, &hint, &res);
	int fd = -1;
	for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd != -1) {
			if (_option(fd) == 0
			 && bind(fd, ai->ai_addr, ai->ai_addrlen) == 0
			 && listen(fd, backlog) == 0) {
				break;
			}
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(res);
	return fd;
}

static struct listener *
listener_new(struct weenet_process *p, uintptr_t data, uintptr_t meta) {
	(void)meta;
	const char *address = (char*)data;
	if (address == NULL) return NULL;
	size_t len = strlen(address);
	int backlog = (int)meta;
	int fd = _listen(address, len, backlog);
	if (fd < 0) {
		weenet_logger_errorf("listen(%s, %d) failed: %s.\n", address, backlog, strerror(errno));
		return NULL;
	}
	struct listener *l = wmalloc(sizeof(*l) + len + 1);
	l->fd = fd;
	l->self = weenet_process_self(p);
	l->forward = NULL;
	memcpy(l->address, address, len+1);
	return l;
}

static void
listener_delete(struct listener *l) {
	close(l->fd);
	wfree(l);
}

static int
listener_handle(struct listener *l, struct weenet_process *p, struct weenet_message *m) {
	int fd = l->fd;
	uint32_t type = weenet_message_type(m);
	switch (type) {
	case WMESSAGE_TYPE_TEXT:
		;struct weenet_process *forward = (void*)m->data;
		if (l->monitor != 0) {
			l->monitor = 0;
			weenet_process_demonitor(p, l->monitor);
			if (forward == NULL) {
				weenet_event_monitor(l->self, 0, fd, WEVENT_DELETE, WEVENT_READ);
			}
		} else if (forward != NULL) {
			weenet_event_monitor(l->self, 0, fd, WEVENT_ADD, WEVENT_READ);
		}
		l->forward = forward;
		if (forward != NULL) {
			l->monitor = weenet_process_monitor(p, forward);
		}
		if ((m->tags & WMESSAGE_FLAG_REQUEST)) {
			weenet_process_send(m->source, l->self, m->session, WMESSAGE_FLAG_RESPONSE, 0, 0);
		}
		break;
	case WMESSAGE_TYPE_RETIRED:
		assert(l->monitor == (monitor_t)m->meta && l->forward == (void*)m->data);
		l->monitor = 0;
		l->forward = NULL;
		weenet_event_monitor(l->self, 0, fd, WEVENT_DELETE, WEVENT_READ);
		break;
	case WMESSAGE_TYPE_EVENT:
		if (l->forward == NULL) return 0;
		for (;;) {
			int conn = accept(fd, NULL, NULL);
			if (conn < 0) {
				int err = errno;
				switch (err) {
				case EINTR:
					break;
				case EAGAIN:
					return 0;
				default:
					weenet_logger_fatalf("accept(%s) error: %s.\n", l->address, strerror(err));
					return -1;
				}
				continue;
			}
			if (_noblocking(conn) == -1) {
				weenet_logger_fatalf("set noblocking mode failed: %s\n", strerror(errno));
			}
			weenet_process_push(l->forward, l->self, 0, WMESSAGE_TYPE_FILE|WMESSAGE_RIDX_FILE, (uintptr_t)conn, 0);
		}
		break;
	default:
		break;
	}
	return 0;
}

const struct weenet_interface listener_service = {
	.new		= (service_new_t)listener_new,
	.handle		= (service_handle_t)listener_handle,
	.delete		= (service_delete_t)listener_delete,
};
