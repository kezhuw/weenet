#include "event.h"
#include "atomic.h"
#include "logger.h"
#include "memory.h"
#include "process.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <unistd.h>
#include <pthread.h>

enum { WEVENT_DISABLED = 0xC0000000 };

#if defined(__linux__)
#include <sys/epoll.h>

#define _disabled(e)		((e) & WEVENT_DISABLED)
#define _recorded(e)		((e) != 0 && !_disabled(e))
#define _mask(e)		((e) & WEVENT_MASK)

struct event_data {
	int fd;
	int lock;
	struct {
		uint32_t event;
		process_t source;
		session_t session;
	} read;
	struct {
		uint32_t event;
		process_t source;
		session_t session;
	} write;
};

#define _lock_data(d)		weenet_atomic_lock(&d->lock)
#define _unlock_data(d)		weenet_atomic_unlock(&d->lock)

#define _lock_event(e)		weenet_atomic_lock(&e->lock)
#define _unlock_event(e)	weenet_atomic_unlock(&e->lock)

static void *
_poll(void *arg) {
	pthread_detach(pthread_self());

	int epfd = (int)(intptr_t)arg;
	struct epoll_event events[256];
	for (;;) {
		int n = epoll_wait(epfd, events, nelem(events), -1);
		if (n < 0) {
			switch (errno) {
			case EBADF:	// epfd is not a valid file descriptor
			case EINVAL:	// epfd is not an epoll file descriptor
				weenet_logger_fatalf("epoll(%d) is down!\n", epfd);
				return NULL;
			case EINTR:
				break;
			default:
				perror("epoll_wait()");
			}
			continue;
		}
		for (int i=0; i<n; ++i) {
			uint32_t e = events[i].events;
			struct event_data *d = events[i].data.ptr;
			struct {
				process_t source;
				session_t session;
			} read = {0, 0};
			struct {
				process_t source;
				session_t session;
			} write = {0, 0};
			int fd = d->fd;
			_lock_data(d);
			uint32_t revent = d->read.event;
			uint32_t wevent = d->write.event;
			if (((e & (EPOLLIN|EPOLLPRI|EPOLLERR|EPOLLHUP|EPOLLRDHUP)) && _recorded(revent))) {
				read.source = d->read.source;
				read.session = d->read.session;
				if ((revent & (WEVENT_ONESHOT|WEVENT_DISPATCH))) {
					if ((revent & WEVENT_DISPATCH)) {
						revent |= WEVENT_DISABLED;
						d->read.event |= WEVENT_DISABLED;
					} else {
						revent = 0;
						d->read.event = 0;
					}
					struct epoll_event ev;
					ev.data.ptr = d;
					if (_recorded(wevent)) {
						ev.events = EPOLLOUT;
						epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
					} else {
						epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
					}
				}
			}
			if ((e & (EPOLLOUT|EPOLLHUP)) && _recorded(wevent)) {
				write.source = d->write.source;
				write.session = d->write.session;
				if ((wevent & (WEVENT_ONESHOT|WEVENT_DISPATCH))) {
					if ((wevent & WEVENT_DISPATCH)) {
						d->write.event |= WEVENT_DISABLED;
					} else {
						d->write.event = 0;
					}
					struct epoll_event ev;
					ev.data.ptr = d;
					if (_recorded(revent)) {
						ev.events = EPOLLIN|EPOLLPRI;
						epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
					} else {
						epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
					}
				}
			}
			_unlock_data(d);
			if (read.source != 0) {
				weenet_process_send(read.source, 0, read.session, WMESSAGE_TYPE_EVENT|WMESSAGE_FLAG_RESPONSE, fd, WEVENT_READ);
			}
			if (write.source != 0) {
				weenet_process_send(write.source, 0, write.session, WMESSAGE_TYPE_EVENT|WMESSAGE_FLAG_RESPONSE, fd, WEVENT_WRITE);
			}
		}
	}
}

struct event {
	int max;
	int size;
	int lock;
	int epfd;
	struct event_data **events;
};

static struct event *E;

int
weenet_event_start(int max) {
	int epfd = epoll_create(max);
	if (epfd < 0) {
		perror("epoll_create(1)");
		return -1;
	}
	struct event *e = wcalloc(sizeof(struct event));
	e->size = max + 1024;
	e->epfd = epfd;
	e->events = wcalloc(sizeof(struct event_data *)*e->size);
	assert(e->events != NULL);

	pthread_t tid;
	int err = pthread_create(&tid, NULL, _poll, (void*)(intptr_t)epfd);
	if (err != 0) {
		errno = err;
		perror("pthread_create() in event");
		close(epfd);
		wfree(e->events);
		wfree(e);
		return -1;
	}

	E = e;
	return 0;
}

int
weenet_event_monitor(process_t source, session_t session, int fd, int op, int event) {
	if (source == 0) return EINVAL;

	struct event *e = E;
	_lock_event(e);
	if (fd >= e->size) {
		size_t size = 2*e->size + 1;
		struct event_data **events = wrealloc(e->events, sizeof(void*)*size);
		if (events == NULL) {
			weenet_atomic_unlock(&e->lock);
			return -1;
		}
		memzero(events+e->size, sizeof(void*)*(size - e->size));
		e->events = events;
		e->size = size;
	}
	struct event_data *d = e->events[fd];
	if (d == NULL) {
		d = wcalloc(sizeof(*d));
		e->events[fd] = d;
		d->fd = fd;
	}
	if (fd > e->max) {
		e->max = fd;
	}
	_unlock_event(e);

	if (_mask(event) != WEVENT_READ && _mask(event) != WEVENT_WRITE) {
		return EINVAL;
	}
	if (op != WEVENT_ADD && op != WEVENT_ENABLE && op != WEVENT_DELETE) {
		return EINVAL;
	}

	int epfd = e->epfd;
	struct epoll_event ev;
	ev.data.ptr = d;
	int err = 0;
	_lock_data(d);
	if (_mask(event) == WEVENT_READ) {
		if (op == WEVENT_DELETE) {
			if (d->read.event == 0) {
				err = ENOENT;
				goto done;
			}
			if (!_disabled(d->read.event)) {
				if (_recorded(d->write.event)) {
					ev.events = EPOLLOUT;
					err = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
				} else {
					err = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
				}
				if (err != 0) {
					err = errno;
				}
			}
			d->read.event = 0;
		} else {
			if (op == WEVENT_ENABLE) {
				if (d->read.event == 0) {
					err = ENOENT;
					goto done;
				} else if (!_disabled(d->read.event)) {
					err = EEXIST;
					goto done;
				}
			} else if (op == WEVENT_ADD) {
				if (d->read.event != 0) {
					err = EEXIST;
					goto done;
				}
			}
			if (_recorded(d->write.event)) {
				ev.events = EPOLLIN | EPOLLPRI | EPOLLOUT;
				err = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
			} else {
				ev.events = EPOLLIN | EPOLLPRI;
				err = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
			}
			if (err != 0) {
				err = errno;
			}
			d->read.event = event;
			d->read.source = source;
			d->read.session = session;
		}
	} else if (_mask(event) == WEVENT_WRITE) {
		if (op == WEVENT_DELETE) {
			if (d->write.event == 0) {
				err = ENOENT;
				goto done;
			}
			if (!_disabled(d->write.event)) {
				if (_recorded(d->read.event)) {
					ev.events = EPOLLIN | EPOLLPRI;
					err = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
				} else {
					err = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
				}
				if (err != 0) {
					err = errno;
				}
			}
			d->write.event = 0;
		} else {
			if (op == WEVENT_ENABLE) {
				if (d->write.event == 0) {
					err = ENOENT;
					goto done;
				} else if (!_disabled(d->write.event)) {
					err = EEXIST;
					goto done;
				}
			} else if (op == WEVENT_ADD) {
				if (d->write.event != 0) {
					err = EEXIST;
					goto done;
				}
			}
			if (_recorded(d->read.event)) {
				ev.events = EPOLLIN | EPOLLPRI | EPOLLOUT;
				err = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
			} else {
				ev.events = EPOLLOUT;
				err = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
			}
			if (err != 0) {
				err = errno;
			}
			d->write.event = event;
			d->write.source = source;
			d->write.session = session;
		}
	}
done:
	_unlock_data(d);
	return err;
}

#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

static int kqfd = -1;

static void *
_poll(void *arg) {
	pthread_detach(pthread_self());

	int fd = (int)(intptr_t)arg;
	struct kevent events[256];
	for (;;) {
		int n = kevent(fd, NULL, 0, events, nelem(events), NULL);
		if (n < 0) {
			switch (errno) {
			// The only fd here is kqueue fd.
			//
			// Calling close() on a file descriptor will remove any
			// kevents that reference the descriptor.   -- kqueue(2)
			case EBADF:
				weenet_logger_fatalf("kqueue(%d) is down!\n", fd);
				return NULL;
			case EINTR:
				break;
			default:
				perror("kevent()");
			}
			continue;
		}
		for (int i=0; i<n; ++i) {
			struct kevent *ev = &events[i];
			uintptr_t event = 0;
			if (ev->filter == EVFILT_READ) {
				event = WEVENT_READ;
			} else if (ev->filter == EVFILT_WRITE) {
				event = WEVENT_WRITE;
			} else {
				weenet_logger_fatalf(
					"unexpected kevent result: ident(%llu) filter(%d) flags(%d) fflags(%u) data(%lld).\n",
					ev->ident, (int)ev->filter, (int)ev->flags, (unsigned)ev->fflags, ev->data);
				continue;
			}
			uintptr_t udata = (uintptr_t)ev->udata;
			process_t source = (process_t)udata;
			session_t session = (session_t)(udata >> 32);
			uintptr_t fd = ev->ident;
			weenet_process_send(source, 0, session, WMESSAGE_TYPE_EVENT|WMESSAGE_FLAG_RESPONSE, fd, event);
		}
	}
	return NULL;
}

int
weenet_event_start(int max) {
	(void)max;
	int fd = kqueue();
	if (fd < 0) {
		perror("kqueue()");
		return -1;
	}
	pthread_t tid;
	int err = pthread_create(&tid, NULL, _poll, (void*)(intptr_t)fd);
	if (err != 0) {
		errno = err;
		perror("pthread_create() in event");
		close(fd);
		return -1;
	}
	kqfd = fd;
	return 0;
}

int
weenet_event_monitor(process_t source, session_t session, int fd, int op, int event) {
	switch(op) {
	case WEVENT_ADD:
		op = EV_ADD;
		break;
	case WEVENT_ENABLE:
		op = EV_ENABLE;
		break;
	case WEVENT_DELETE:
		op = EV_DELETE;
		break;
	default:
		return EINVAL;
	}

	int filter = 0;
	switch (event & WEVENT_MASK) {
	case WEVENT_READ:
		filter = EVFILT_READ;
		break;
	case WEVENT_WRITE:
		filter = EVFILT_WRITE;
		break;
	default:
		return EINVAL;
	}
	if ((event & WEVENT_CLEAR)) {
		op |= EV_CLEAR;
	}
	if ((event & WEVENT_DISPATCH)) {
		op |= EV_DISPATCH;
	}

	void *udata = (void*)((uintptr_t)source | ((uintptr_t)session << 32));
	struct kevent ev;
	EV_SET(&ev, fd, (short)filter, (u_short)op, 0, 0, udata);
	kevent(kqfd, &ev, 1, NULL, 0, NULL);
	return 0;
}
#endif
