#define _BSD_SOURCE

#include "timer.h"
#include "memory.h"
#include "process.h"
#include "service.h"

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/limits.h>	// for PATH_MAX
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

struct logger {
	int fd;
	int seq;
	size_t size;
	size_t fsize;
	size_t limit;
	bool creating;
	char path[PATH_MAX];
	size_t len;
	char dir[];	// dir/seq-create_time.i.log
};

#define TS_MAX		24
#define TS_FMT		"%F-%T"

static size_t
_now(char *buf, size_t len) {
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	return strftime(buf, len, TS_FMT, &tm);
}

static int
_open(struct logger *l) {
	char ts[TS_MAX];
	_now(ts, sizeof(ts));
	int seq = l->seq + 1;
	for (int i=0; ; ++i) {
		size_t n = snprintf(l->path, sizeof(l->path), "%s/%d.%s.%d.log", l->dir, seq, ts, i);
		if (n >= sizeof(l->path)) {
			return -1;
		}
		for (;;) {
			int fd = open(l->path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
			if (fd < 0) {
				switch (errno) {
				case EEXIST:
					fprintf(stderr, "open(%s) existed.\n", l->path);
					goto nexti;
				case EINTR:
					continue;	// open again
				default:
					fprintf(stderr, "open(%s) creating log file failed[%s].\n", l->path, strerror(errno));
					return -1;
				}
			}
			l->seq = seq;
			return fd;
		}
nexti:
		;
	}
	return -1;
}

static void
_close(int fd) {
	fsync(fd);
	close(fd);
}

static void
_delete(struct logger *l) {
	if (l->fd != -1) {
		_close(l->fd);
	}
	wfree(l);
}

static struct logger *
logger_new(struct weenet_process *p, uintptr_t data, uintptr_t meta) {
	(void)p;
	const char *base = (char*)data;
	if (base == NULL || base[0] == '\0') return NULL;

	size_t len = strlen(base);
	struct logger *l = wmalloc(sizeof(*l)+len+1);
	memcpy(l->dir, base, len);
	if (base[len-1] == '/' && len != 1) {
		l->len = len-1;
	} else {
		l->len = len;
	}
	l->dir[l->len] = '\0';
	l->fd = -1;
	l->seq = 0;
	l->size = 0;
	l->fsize = 0;
	l->limit = ((size_t)meta == 0) ? 1024*1024*100/*100M*/: (size_t)meta;
	l->creating = false;

	int fd = _open(l);
	if (fd < 0) {
		_delete(l);
		return NULL;
	}
	l->fd = fd;
	return l;
}

static void
logger_delete(struct logger *l, struct weenet_process *p) {
	(void)p;
	_delete(l);
}

static int
logger_handle(struct logger *l, struct weenet_process *p, struct weenet_message *m) {
	(void)p;
	uint32_t type = weenet_message_type(m);

	switch (type) {
	case WMESSAGE_TYPE_TIMEO:
		if (l->creating) {
			int fd = _open(l);
			if (fd < 0) {
				weenet_process_timeo(p, 1000);
				return 0;
			}
			_close(l->fd);
			l->fd = fd;
			l->fsize = 0;
			l->creating = false;
		}
		break;
	case WMESSAGE_TYPE_TEXT:
		if (m->meta == 0) {
			return 0;
		}
		char buf[TS_MAX];
		size_t len = _now(buf, sizeof buf);
		struct iovec v[3];
		size_t n = 2;
		v[0].iov_base = buf;
		v[0].iov_len = len;
		v[1].iov_base = (void*)m->data;
		v[1].iov_len = (size_t)m->meta;
		size_t size = len + (size_t)m->meta;
		char newline[1] = {'\n'};
		if (((char*)m->data)[(size_t)m->meta - 1] != '\n') {
			v[2].iov_base = newline;
			v[2].iov_len = 1;
			n = 3;
			++size;
		}
		ssize_t wr = writev(l->fd, v, n);
		if (wr < 0 || (size_t)wr != size) {
			fprintf(stderr, "weenet_logger FATAL writev(%d, %zu) return(%ld) errno(%d %s)\n",
				l->fd, size, (long)wr, errno, strerror(errno));
			return -1;
		}
		l->size += size;
		l->fsize += size;
		if (l->fsize >= l->limit && !l->creating) {
			l->creating = true;
			weenet_process_timeo(p, 1);
		}
		break;
	case WMESSAGE_TYPE_RETIRE:
		break;
	default:
		return -1;
	}

	return 0;
}

const struct weenet_interface logger_service = {
	.new		= (service_new_t)logger_new,
	.handle		= (service_handle_t)logger_handle,
	.delete		= (service_delete_t)logger_delete,
};
