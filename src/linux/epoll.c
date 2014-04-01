#include <sys/epoll.h>

enum { WEVENT_DISABLED = 0xC0000000 };

#define _disabled(e)		((e) & WEVENT_DISABLED)
#define _recorded(e)		((e) != 0 && !_disabled(e))
#define _mask(e)		((e) & WEVENT_MASK)

struct filter {
	uint32_t event;
	process_t source;
	session_t session;
};

enum {
	EPOLL_READ		= EPOLLIN | EPOLLPRI | EPOLLET,
	EPOLL_WRITE		= EPOLLOUT | EPOLLET,

	EPOLL_READ_MASK		= EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLRDHUP,
	EPOLL_WRITE_MASK	= EPOLLOUT | EPOLLHUP,
};

static uint32_t MASKS[2] = { EPOLL_READ_MASK, EPOLL_WRITE_MASK };

static int EVENTS[2] = { WEVENT_READ, WEVENT_WRITE };
static int INDICES[] = { [WEVENT_READ] = 0, [WEVENT_WRITE] = 1 };

struct file {
	int fd;
	int lock;
	int pending;
	struct filter filters[2];
};

#define _lock_file(f)		weenet_atomic_lock(&f->lock)
#define _unlock_file(f)		weenet_atomic_unlock(&f->lock)

inline static void
_init(struct file *f, int fd) {
	f->fd = fd;
	f->pending = 0;
	f->filters[0].event = 0;
	f->filters[1].event = 0;
}

inline static void
_clear(struct file *f) {
	f->fd = -1;
}

#define _lock_event(e)		weenet_atomic_lock(&e->lock)
#define _unlock_event(e)	weenet_atomic_unlock(&e->lock)

inline static void
_send(process_t pid, session_t session, int fd, int event) {
	uint32_t flag = session == 0 ? 0 : WMESSAGE_FLAG_RESPONSE;
	weenet_process_send(pid, 0, session, WMESSAGE_TYPE_EVENT|flag, (uintptr_t)fd, (uintptr_t)event);
}

static void
_report(struct file *f, uint32_t events, int epfd) {
	struct {
		process_t source;
		session_t session;
	} fires[] = { {0,0}, {0,0} };

	_lock_file(f);

	int fd = f->fd;
	if (fd == -1) {
		_unlock_file(f);
		return;
	}

	for (int index = 0; index <= 1; ++index) {
		if ((events & MASKS[index])) {
			uint32_t event = f->filters[index].event;
			if (_recorded(event)) {
				fires[index].source = f->filters[index].source;
				fires[index].session = f->filters[index].session;
				if ((event & WEVENT_ONESHOT)) {
					f->filters[index].event = 0;
				} else if ((event & WEVENT_DISPATCH)) {
					f->filters[index].event |= WEVENT_DISABLED;
				}
			} else {
				f->pending |= EVENTS[index];
			}
		}
	}

	if (f->filters[0].event == 0 && f->filters[1].event == 0) {
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
		_clear(f);
	}

	_unlock_file(f);

	for (int index = 0; index <= 1; ++index) {
		_send(fires[index].source, fires[index].session, fd, EVENTS[index]);
	}
}

inline static int
_pending(int *pending, int event) {
	if ((*pending & event)) {
		*pending &= ~event;
		return event;
	}
	return 0;
}

static int
_monitor(int epfd, process_t source, session_t session, int op, int event, int fd, struct file *f) {
	int pending = 0;
	int index = INDICES[_mask(event)];

	_lock_file(f);

	if (f->fd == -1) {
		if (op != WEVENT_ADD) {
			_unlock_file(f);
			return  EINVAL;
		}

		_init(f, fd);
		f->filters[index].event = (uint32_t)event;
		f->filters[index].source = source;
		f->filters[index].session = session;

		struct epoll_event ev;
		ev.events = EPOLL_READ | EPOLL_WRITE;
		ev.data.ptr = f;
		int err = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
		if (err != 0) {
			err = errno;
			assert(err != EEXIST);
			_clear(f);
		}
		_unlock_file(f);
		return err;
	}

	assert(f->fd == fd);

	switch (op) {
	case WEVENT_ADD:
		f->filters[index].event = (uint32_t)event;
		f->filters[index].source = source;
		f->filters[index].session = session;
		pending = _pending(&f->pending, _mask(event));
		break;
	case WEVENT_ENABLE:
		if (!_disabled(f->filters[index].event)) {
			break;
		}
		f->filters[index].event &= ~WEVENT_DISABLED;
		f->filters[index].source = source;
		f->filters[index].session = session;
		pending = _pending(&f->pending, _mask(event));
		break;
	case WEVENT_DELETE:
		f->filters[index].event = 0;
		if (f->filters[0].event == 0 && f->filters[1].event == 0) {
			epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
			_clear(f);
		}
		break;
	}

	_unlock_file(f);

	if (pending != 0) {
		_send(source, session, fd, pending);
	}

	return 0;
}

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
			_report(events[i].data.ptr, events[i].events, epfd);
		}
	}
	return NULL;
}

struct event {
	int max;
	size_t size;
	int lock;
	int epfd;
	struct file **files;
};

static struct event *E;

int
weenet_event_start(int max) {
	int epfd = epoll_create(1);
	if (epfd < 0) {
		perror("epoll_create(1)");
		return -1;
	}

	struct event *e = wcalloc(sizeof(struct event));
	e->size = (size_t)max + 1024;
	e->epfd = epfd;
	e->files = wcalloc(sizeof(struct file *)*e->size);
	assert(e->files != NULL);

	pthread_t tid;
	int err = pthread_create(&tid, NULL, _poll, (void*)(intptr_t)epfd);
	if (err != 0) {
		fprintf(stderr, "pthread_create() in event_start: %s\n", strerror(err));
		close(epfd);
		wfree(e->files);
		wfree(e);
		return -1;
	}

	E = e;
	return 0;
}

int
weenet_event_monitor(process_t source, session_t session, int fd, int op, int event) {
	if (source == 0) return EINVAL;

	int filter = _mask(event);
	if (filter != WEVENT_READ && filter != WEVENT_WRITE) {
		return EINVAL;
	}
	if (op != WEVENT_ADD && op != WEVENT_ENABLE && op != WEVENT_DELETE) {
		return EINVAL;
	}

	struct event *e = E;
	_lock_event(e);
	if ((size_t)fd >= e->size) {
		size_t size = 2*e->size + 1;
		struct file **files = wrealloc(e->files, sizeof(void*)*size);
		if (files == NULL) {
			_unlock_event(e);
			return -1;
		}
		memzero(files+e->size, sizeof(void*)*(size - e->size));
		e->files = files;
		e->size = size;
	}
	struct file *f = e->files[fd];
	if (f == NULL) {
		// TODO custom allocator
		f = wmalloc(sizeof(*f));
		e->files[fd] = f;
		f->fd = -1;
		f->lock = 0;
	}
	if (fd > e->max) {
		e->max = fd;
	}
	_unlock_event(e);

	return _monitor(e->epfd, source, session, op, event, fd, f);
}
