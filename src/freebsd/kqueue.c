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
					"unexpected kevent result: ident(%lu) filter(%d) flags(%d) fflags(%u) data(%ld).\n",
					ev->ident, (int)ev->filter, (int)ev->flags, (unsigned)ev->fflags, ev->data);
				continue;
			}
			uintptr_t udata = (uintptr_t)ev->udata;
			process_t source = (process_t)udata;
			session_t session = (session_t)(udata >> 32);
			uintptr_t fd = ev->ident;
			_send(source, session, fd, event);
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

	op |= EV_CLEAR;
	if (session != 0) {
		op |= EV_ONESHOT;
	}

	void *udata = (void*)((uintptr_t)source | ((uintptr_t)session << 32));
	struct kevent ev;
	EV_SET(&ev, fd, (short)filter, (u_short)op, 0, 0, udata);
	kevent(kqfd, &ev, 1, NULL, 0, NULL);
	return 0;
}
