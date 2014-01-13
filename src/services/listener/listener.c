#include "event.h"
#include "logger.h"
#include "memory.h"
#include "process.h"
#include "service.h"
#include "utils.h"
#include "compat.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_SOCKETS = 16 };

struct listener {
	process_t self;
	monitor_t monitor;
	struct weenet_process *forward;
	int nsocket;
	int sockets[MAX_SOCKETS];
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

static const char *
_getaddrinfo(int family, const char *host, const char *port, struct addrinfo **res) {
	struct addrinfo hint;
	memzero(&hint, sizeof hint);
	hint.ai_family = family;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_protocol = IPPROTO_TCP;
	hint.ai_flags = AI_PASSIVE;

	*res = NULL;
	int err = getaddrinfo(host, port, &hint, res);
	switch (err) {
	case 0:
		break;
	case EAI_SYSTEM:
		return strerror(errno);
	default:
		return gai_strerror(err);
	}
	return (*res == NULL) ? "no match addrinfo" : NULL;
}

static const char *
_listen(const char *address, size_t len, int backlog, int *nsocket, int sockets[MAX_SOCKETS]) {
	char tmp[len+1];
	memcpy(tmp, address, len+1);
	address = tmp;

	char *host = NULL;
	char *port = NULL;
	int family = AF_UNSPEC;
	int err = _parse(tmp, len, &family, &host, &port);
	if (err != 0) {
		return "illegal address format";
	}

	struct addrinfo *res;
	const char *error = _getaddrinfo(family, host, port, &res);
	if (error != NULL) {
		return error;
	}

	int n = 0;
	for (struct addrinfo *ai = res; ai != NULL && n < MAX_SOCKETS; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd != -1) {
			if (_option(fd) == 0
			 && bind(fd, ai->ai_addr, ai->ai_addrlen) == 0
			 && listen(fd, backlog) == 0) {
				sockets[n++] = fd;
				continue;
			}
			close(fd);
		}
	}
	freeaddrinfo(res);
	*nsocket = n;

	return n == 0 ? "failed to listen" : NULL;
}

static void
_monitor(struct listener *l, int op) {
	process_t self = l->self;
	for (int i=0, n=l->nsocket; i<n; ++i) {
		int fd = l->sockets[i];
		weenet_event_monitor(self, 0, fd, op, WEVENT_READ);
	}
}

static void
_close(struct listener *l) {
	for (int i=0, n=l->nsocket; i<n; ++i) {
		close(l->sockets[i]);
	}
}

static int
_accept(struct listener *l, int fd) {
	for (;;) {
		int conn = accept(fd, NULL, NULL);
		if (conn < 0) {
			int err = errno;
			switch (err) {
			case EINTR:
				continue;
			case EAGAIN:
				return 0;
			default:
				weenet_logger_fatalf("accept(%s) error: %s.\n", l->address, strerror(err));
				return -1;
			}
		}
		if (_noblocking(conn) == -1) {
			weenet_logger_fatalf("set noblocking mode failed: %s\n", strerror(errno));
		}
		weenet_process_push(l->forward, l->self, 0, WMESSAGE_TYPE_FILE|WMESSAGE_RIDX_FILE, (uintptr_t)conn, 0);
	}
	return 0;
}

static struct listener *
listener_new(struct weenet_process *p, uintptr_t data, uintptr_t meta) {
	(void)meta;
	const char *address = (char*)data;
	if (address == NULL) return NULL;
	size_t len = strlen(address);
	int backlog = (int)meta;
	int nsocket;
	int sockets[MAX_SOCKETS];
	const char *error = _listen(address, len, backlog, &nsocket, sockets);
	if (error != NULL) {
		weenet_logger_errorf("listen(%s, %d) failed: %s.\n", address, backlog, error);
		return NULL;
	}
	struct listener *l = wmalloc(sizeof(*l) + len + 1);
	l->self = weenet_process_pid(p);
	l->monitor = 0;
	l->forward = NULL;
	l->nsocket = nsocket;
	memcpy(l->sockets, sockets, sizeof(int) * MAX_SOCKETS);
	memcpy(l->address, address, len+1);
	return l;
}

static void
listener_delete(struct listener *l) {
	_close(l);
	wfree(l);
}

static int
listener_handle(struct listener *l, struct weenet_process *p, struct weenet_message *m) {
	uint32_t type = weenet_message_type(m);
	switch (type) {
	case WMESSAGE_TYPE_TEXT:
		;struct weenet_process *forward = (void*)m->data;
		if (l->monitor != 0) {
			l->monitor = 0;
			weenet_process_demonitor(p, l->monitor);
			if (forward == NULL) {
				_monitor(l, WEVENT_DELETE);
			}
		} else if (forward != NULL) {
			_monitor(l, WEVENT_ADD);
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
		_monitor(l, WEVENT_DELETE);
		break;
	case WMESSAGE_TYPE_EVENT:
		if (l->forward == NULL) return 0;
		int fd = (int)m->data;
		return _accept(l, fd);
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
