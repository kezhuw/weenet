#include <sys/epoll.h>

enum { WEVENT_DISABLED = 0xC0000000 };

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

enum {
	EPOLL_READ		= EPOLLIN | EPOLLPRI | EPOLLET,
	EPOLL_WRITE		= EPOLLOUT | EPOLLET,

	EPOLL_READ_MASK		= EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLRDHUP,
	EPOLL_WRITE_MASK	= EPOLLOUT | EPOLLHUP,
};

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
			if ((e & EPOLL_READ_MASK) && _recorded(revent)) {
				read.source = d->read.source;
				read.session = d->read.session;
				if ((revent & (WEVENT_ONESHOT|WEVENT_DISPATCH))) {
					if ((revent & WEVENT_ONESHOT)) {
						revent = 0;
						d->read.event = 0;
					} else {
						revent |= WEVENT_DISABLED;
						d->read.event |= WEVENT_DISABLED;
					}
					struct epoll_event ev;
					ev.data.ptr = d;
					if (_recorded(wevent)) {
						ev.events = EPOLL_WRITE;
						epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
					} else {
						epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
					}
				}
			}
			if ((e & EPOLL_WRITE_MASK) && _recorded(wevent)) {
				write.source = d->write.source;
				write.session = d->write.session;
				if ((wevent & (WEVENT_ONESHOT|WEVENT_DISPATCH))) {
					if ((wevent & WEVENT_ONESHOT)) {
						d->write.event = 0;
					} else {
						d->write.event |= WEVENT_DISABLED;
					}
					struct epoll_event ev;
					ev.data.ptr = d;
					if (_recorded(revent)) {
						ev.events = EPOLL_READ;
						epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
					} else {
						epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
					}
				}
			}
			_unlock_data(d);
			if (read.source != 0) {
				weenet_process_send(read.source, 0, read.session, WMESSAGE_TYPE_EVENT|WMESSAGE_FLAG_RESPONSE, (uintptr_t)fd, WEVENT_READ);
			}
			if (write.source != 0) {
				weenet_process_send(write.source, 0, write.session, WMESSAGE_TYPE_EVENT|WMESSAGE_FLAG_RESPONSE, (uintptr_t)fd, WEVENT_WRITE);
			}
		}
	}
}

struct event {
	int max;
	size_t size;
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
	e->size = (size_t)max + 1024;
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

	if (_mask(event) != WEVENT_READ && _mask(event) != WEVENT_WRITE) {
		return EINVAL;
	}
	if (op != WEVENT_ADD && op != WEVENT_ENABLE && op != WEVENT_DELETE) {
		return EINVAL;
	}

	struct event *e = E;
	_lock_event(e);
	if ((size_t)fd >= e->size) {
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
					ev.events = EPOLL_WRITE;
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
				ev.events = EPOLL_READ | EPOLL_WRITE;
				err = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
			} else {
				ev.events = EPOLL_READ;
				err = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
			}
			if (err != 0) {
				err = errno;
			}
			d->read.event = (uint32_t)event;
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
					ev.events = EPOLL_READ;
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
				ev.events = EPOLL_READ | EPOLL_WRITE;
				err = epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
			} else {
				ev.events = EPOLL_WRITE;
				err = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
			}
			if (err != 0) {
				err = errno;
			}
			d->write.event = (uint32_t)event;
			d->write.source = source;
			d->write.session = session;
		}
	}
done:
	_unlock_data(d);
	return err;
}
