#include "process.h"

#include "atom.h"
#include "atomic.h"
#include "logger.h"
#include "memory.h"
#include "service.h"
#include "schedule.h"
#include "slab.h"
#include "timer.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>	// for free()
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>	// for close()

enum {
	WMESSAGE_TAGS_MONITOR		= WMESSAGE_TYPE_MONITOR | WMESSAGE_FLAG_INTERNAL,
	WMESSAGE_TAGS_RETIRED		= WMESSAGE_TYPE_RETIRED | WMESSAGE_RIDX_PROC | WMESSAGE_FLAG_INTERNAL,
};

struct weenet_mailbox {
	uint32_t num;
	uint32_t size;
	uint32_t head;
	uint32_t rear;
	int32_t lock;
	int32_t active;
	struct weenet_message **mbox;
};

struct weenet_monitor {
	uint32_t num;
	uint32_t len;
	struct _monitor {
		monitor_t mref;
		uintptr_t pref;	// pid or pointer to process
	} *monitors;
};

struct weenet_process {
	process_t id;
	session_t session;
	struct {
		process_t source;
		session_t session;
	} wait;
	const char *name;
	struct weenet_service *service;
	struct weenet_mailbox mailbox;

	uint32_t mref;		// monitor reference, just/almost unique in this process.
	int32_t refcnt;
	bool retired;	// integer ?
	struct weenet_monitor supervisors;	// processes that monitoring this process
	struct weenet_monitor supervisees;	// processes that this process monitoring
};

#define _pid(p)	((p)->id)
#define _name(p)	((p)->name)

static /*__thread*/ struct slab *process_slab;
static struct slab *message_slab;

static void
_file_resource_release(void *ud, uintptr_t data, uintptr_t meta) {
	(void)ud; (void)meta;
	int fd = (int)data;
	close(fd);
}

static void
_rawmem_resource_release(void *ud, uintptr_t data, uintptr_t meta) {
	(void)ud; (void)meta;
	free((void*)data);
}

static void
_memory_resource_release(void *ud, uintptr_t data, uintptr_t meta) {
	(void)ud; (void)meta;
	wfree((void*)data);
}

static void
_process_resource_release(void *ud, uintptr_t data, uintptr_t meta) {
	(void)ud; (void)meta;
	struct weenet_process *p = (void*)data;
	weenet_process_release(p);
}

static struct {
	struct {
		void *ud;
		resource_fini_t fn;
	} pairs[WMESSAGE_RIDX_MASK+1];
} F;

#define _ridx(tags)		((tags) & WMESSAGE_RIDX_MASK)
#define _migrated(tags)		((tags) & WMESSAGE_FLAG_MIGRATED)

static void
_reclaim_resource(uint32_t tags, uintptr_t data, uintptr_t meta) {
	if (_migrated(tags)) {
		return;
	}
	uint32_t ridx = _ridx(tags);
	resource_fini_t fn = F.pairs[ridx].fn;
	if (fn != NULL) {
		fn(F.pairs[ridx].ud, data, meta);
	}
}

int
weenet_message_gc(uint32_t id, void *ud, resource_fini_t fn) {
	if (id == 0) return EINVAL;
	if (id > WMESSAGE_RIDX_MASK) return ERANGE;
	// Sufficent to protect (ud, fn) pair in F.
	//
	// In a single process, registration happens before
	// sending a message with 'id', which happens before
	// deletion of the message.
	if (!weenet_atomic_cas(&F.pairs[id].fn, NULL, fn)) return EEXIST;
	F.pairs[id].ud = ud;
	return 0;
}

struct weenet_message *
weenet_message_new(process_t source, process_t session, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_message *msg = slab_retain(message_slab);
	msg->source = source;
	msg->session = session;
	msg->data = data;
	msg->meta = meta;
	msg->tags = tags;
	msg->refcnt = 1;
	return msg;
}

void
weenet_message_delete(struct weenet_message *m) {
	_reclaim_resource(m->tags, m->data, m->meta);
	slab_release(message_slab, m);
}

// XXX Multicast message may exist race condition ?
void
weenet_message_take(struct weenet_message *msg) {
	msg->tags |= WMESSAGE_FLAG_MIGRATED;
}

struct weenet_message *
weenet_message_ref(struct weenet_message *msg) {
	weenet_atomic_inc(&msg->refcnt);
	return msg;
}

void
weenet_message_copy(struct weenet_message *msg, int n) {
	int32_t refcnt = weenet_atomic_add(&msg->refcnt, n);
	assert(refcnt >= 0);
	if (refcnt == 0) {
		weenet_message_delete(msg);
	}
}

void
weenet_message_unref(struct weenet_message *msg) {
	weenet_message_copy(msg, -1);
}

// Notify supervisors that 'p' is about to be deleted.
static void
weenet_monitor_notify(struct weenet_monitor *m, struct weenet_process *p) {
	uintreg_t n = (uintreg_t)m->num;
	struct _monitor *monitors = m->monitors;
	m->monitors = NULL;
	m->num = m->len = 0;
	if (n != 0) {
		weenet_atomic_add(&p->refcnt, (int32_t)n);
		process_t self = _pid(p);
		for (uintreg_t i=0; i<n; ++i) {
			process_t pid = (process_t)monitors[i].pref;
			weenet_process_send(pid, self, 0, WMESSAGE_TAGS_RETIRED, (uintptr_t)p, monitors[i].mref);
		}
	}
	if (monitors != NULL) {
		wfree(monitors);
	}
}

static void
weenet_monitor_insert(struct weenet_monitor *m, monitor_t mref, uintptr_t pref) {
	uint32_t n = m->num++;
	if (n == m->len) {
		m->len = 2*n + 1;
		m->monitors = wrealloc(m->monitors, m->len * sizeof(m->monitors[0]));
	}
	m->monitors[n].mref = mref;
	m->monitors[n].pref = pref;
}

static bool
weenet_monitor_remove(struct weenet_monitor *m, monitor_t mref, uintptr_t pref) {
	for (intreg_t i=0, last=(intreg_t)m->num-1; i<=last; ++i) {
		if (m->monitors[i].mref == mref && m->monitors[i].pref == pref) {
			if (i != last) {
				m->monitors[i].mref = m->monitors[last].mref;
				m->monitors[i].pref = m->monitors[last].pref;
			}
			m->num = (uint32_t)last;
			return true;
		}
	}
	return false;
}

static struct weenet_process *
weenet_monitor_erase(struct weenet_monitor *m, monitor_t mref) {
	for (intreg_t i=0, last=(intreg_t)m->num-1; i<=last; ++i) {
		if (m->monitors[i].mref == mref) {
			struct weenet_process *p = (struct weenet_process *)m->monitors[i].pref;
			if (i != last) {
				m->monitors[i].mref = m->monitors[last].mref;
				m->monitors[i].pref = m->monitors[last].pref;
			}
			m->num = (uint32_t)last;
			return p;
		}
	}
	return NULL;
}

static void
weenet_monitor_unlink(struct weenet_monitor *m, struct weenet_process *p) {
	uint32_t n = m->num;
	for (uint32_t i=0; i<n; ++i) {
		struct weenet_process *dst = (struct weenet_process *)m->monitors[i].pref;
		weenet_process_push(dst, _pid(p), 0, WMESSAGE_TAGS_MONITOR, 0, (uintptr_t)m->monitors[i].mref);
		weenet_process_release(dst);
	}
	wfree(m->monitors);
	m->monitors = NULL;
	m->num = m->len = 0;
}

static void
weenet_mailbox_expand(struct weenet_mailbox *b) {
	assert(weenet_atomic_locked(&b->lock));
	assert(b->num == b->size);
	uint32_t size = b->size;
	uint32_t newsize = 2*size + 1;
	b->mbox = wrealloc(b->mbox, sizeof(void*) * newsize);
	// Mailbox is full, two situations:
	//
	//   1) ----- head(rear) --------
	//   2) head(0) ------ rear(size)	rare, need no special handling
	uint32_t rear = b->rear;
	if (rear == b->head) {
		// Situation 1.
		assert(b->head == b->rear);
		if (rear < size/2) {
			// Copy (0 <---> rear) to (size <---> size+rear).
			memcpy(b->mbox+size, b->mbox, sizeof(void*)*rear);
			b->rear = size + rear;
		} else {
			// Copy (head <---> size) to rear.
			uint32_t head = rear;
			uint32_t n = size - head;
			uint32_t newhead = newsize - n;
			memcpy(b->mbox + newhead, b->mbox + head, sizeof(void*)*n);
			b->head = newhead;
		}
	}
	b->size = newsize;
}

inline static void
weenet_mailbox_init(struct weenet_mailbox *b) {
	b->active = true;
}

inline static uint32_t
weenet_mailbox_num(struct weenet_mailbox *b) {
	return b->num;
}

static struct weenet_message *
weenet_mailbox_pop(struct weenet_mailbox *b) {
	weenet_atomic_lock(&b->lock);
	assert(b->active);		// Can't pop up message when inactive.
	if (b->num == 0) {
		b->active = false;
		weenet_atomic_unlock(&b->lock);
		return NULL;
	}
	uint32_t head = b->head == b->size ? 0 : b->head;
	struct weenet_message *msg = b->mbox[head++];
	b->head = head;
	b->num -= 1;
	weenet_atomic_unlock(&b->lock);
	return msg;
}

static bool
weenet_mailbox_push(struct weenet_mailbox *b, struct weenet_message *m) {
	weenet_atomic_lock(&b->lock);
	if (b->num == b->size) {
		weenet_mailbox_expand(b);
	}
	uint32_t rear = b->rear == b->size ? 0 : b->rear;
	b->mbox[rear++] = m;
	b->rear = rear;
	b->num += 1;
	bool sleeping = !b->active;
	if (sleeping) {
		b->active = true;
	}
	weenet_atomic_unlock(&b->lock);
	return sleeping;
}

static void
weenet_mailbox_insert(struct weenet_mailbox *b, struct weenet_message *m) {
	//assert(b->active);		// Must be active. There is no reader.
	weenet_atomic_lock(&b->lock);	// lock is needed, there are many writers.
	if (b->num == b->size) {
		weenet_mailbox_expand(b);
	}
	b->head = b->head == 0 ? b->size-1 : b->head-1;
	b->mbox[b->head] = m;
	b->num += 1;
	weenet_atomic_unlock(&b->lock);
}

static void
weenet_mailbox_cleanup(struct weenet_mailbox *b) {
	weenet_atomic_lock(&b->lock);
	struct weenet_message **mbox = b->mbox;
	uintreg_t num = (uintreg_t)b->num;
	if (num == 0) goto done;
	uintreg_t head = (uintreg_t)b->head;
	uintreg_t rear = (uintreg_t)b->rear;
	if (head < rear) {
		do {
			weenet_message_unref(mbox[head]);
		} while (++head < rear);
	} else if (head > rear) {
		for (uintreg_t i=0; i<rear; ++i) {
			weenet_message_unref(mbox[i]);
		}
		for (uintreg_t size = (uintreg_t)b->size; head < size; ++head) {
			weenet_message_unref(mbox[head]);
		}
	} else {
		assert(num == (uintreg_t)b->size);
		for (uintreg_t i=0; i<num; ++i) {
			weenet_message_unref(mbox[i]);
		}
	}
done:
	b->num = 0;
	b->size = 0;
	b->mbox = NULL;
	b->head = b->rear = 0;
	weenet_atomic_unlock(&b->lock);
	wfree(mbox);
}

struct weenet_account {
	size_t len;
	size_t size;
	int64_t lock;
	struct {
		int64_t number;
		intreg_t first;
		intreg_t last;
	} free;
	struct {
		struct weenet_process *proc;
		intreg_t next;
	} *slot;
};

void weenet_process_retire(struct weenet_process *p);

static struct weenet_account *T;

static process_t
weenet_account_enroll(struct weenet_process *p) {
	++p->refcnt;
	struct weenet_account *t = T;
	weenet_atomic_lock(&t->lock);
	if (t->len == t->size) {
		intreg_t index = t->free.first;
		if (index != -1) {
			assert(t->slot[index].proc == NULL);
			t->free.first = t->slot[index].next;
			t->slot[index].proc = p;
			weenet_atomic_unlock(&t->lock);
			return (process_t)index;
		} else {
			size_t size = t->size;
			size += size/2+1;
			t->slot = wrealloc(t->slot, size * sizeof(t->slot[0]));
			t->size = size;
		}
	}
	assert(t->len < t->size);
	size_t index = ++t->len;
	t->slot[index].proc = p;
	weenet_atomic_unlock(&t->lock);
	return (process_t)index;
}

static void
weenet_account_unlink(process_t pid) {
	if (pid == PROCESS_ZERO) return;
	struct weenet_account *t = T;
	weenet_atomic_lock(&t->lock);
	intreg_t index = (intreg_t)pid;
	assert((size_t)index <= t->len);
	if (t->slot[index].proc == NULL) {
		weenet_atomic_unlock(&t->lock);
		return;
	}
	t->slot[index].proc = NULL;
	t->slot[index].next = -1;
	if (t->free.first == -1) {
		t->free.first = t->free.last = index;
	} else {
		assert(t->slot[t->free.last].next == -1);
		t->slot[t->free.last].next = index;
		t->free.last = index;
	}
	weenet_atomic_unlock(&t->lock);
}

struct weenet_process *
weenet_account_retain(process_t pid) {
	struct weenet_account *t = T;
	weenet_atomic_lock(&t->lock);
	assert((size_t)pid <= t->len);
	struct weenet_process *p = t->slot[pid].proc;
	if (p != NULL) {
		if (p->refcnt == 0) {
			weenet_atomic_unlock(&t->lock);
			return NULL;
		}
		// retained under lock
		int32_t refcnt = weenet_atomic_add(&p->refcnt, 1);
		if (refcnt == 1) {
			// Someone just released it, about to delete it.
			weenet_atomic_sub(&p->refcnt, 1);
			weenet_atomic_unlock(&t->lock);
			return NULL;
		}
	}
	weenet_atomic_unlock(&t->lock);
	return p;
}

void
weenet_account_retire(process_t pid) {
	struct weenet_process *p = weenet_account_retain(pid);
	if (p == NULL) return;
	weenet_process_retire(p);
	weenet_process_release(p);
}

// Return retained process, named with 'name'.
struct weenet_process *
weenet_account_search(struct weenet_atom *name) {
	(void)name;
	// FIXME
	return PROCESS_ZERO;
}

bool
weenet_account_register(process_t pid, struct weenet_atom *name) {
	(void)pid; (void)name;
	// FIXME
	return false;
}

bool
weenet_account_unregister(struct weenet_atom *name) {
	(void)name;
	// FIXME
	return true;
}

// Emphasize that the name is an atom, so it is safe to use without worrying its lifetime.
static struct weenet_process *
_process_new(struct weenet_atom *atom) {
	struct weenet_process *p = slab_retain(process_slab);
	memzero(p, sizeof(*p));
	p->name = weenet_atom_str(atom);
	p->refcnt = 1;
	weenet_mailbox_init(&p->mailbox);
	return p;
}

static void
_process_delete(struct weenet_process *p) {
	slab_release(process_slab, p);
}

__thread struct weenet_process *_running;

inline static struct weenet_process *
_running_process() {
	return _running;
}

inline static void
_set_running_process(struct weenet_process *p) {
	_running = p;
}

static bool weenet_process_work(struct weenet_process *p);

struct weenet_process *
weenet_process_new(const char *name, uintptr_t data, uintptr_t meta) {
	struct weenet_atom *atom = weenet_atom_new(name, strlen(name));
	struct weenet_process *p = _process_new(atom);
	p->id = weenet_account_enroll(p);
	struct weenet_process *running = _running_process();
	_set_running_process(p);
	p->service = weenet_service_new(atom, p, data, meta);
	if (p->service == NULL) {
		fprintf(stderr, "failed to start new process [%s].\n", name);
		p->retired = true;
		weenet_monitor_notify(&p->supervisors, p);
		weenet_process_release(p);
		weenet_process_release(p);
		_set_running_process(running);
		return NULL;
	}
	if (weenet_process_work(p)) {
		weenet_schedule_resume(p);
	}
	_set_running_process(running);
	return p;
}

static void
weenet_process_delete(struct weenet_process *p) {
	weenet_account_unlink(p->id);
	assert(weenet_atomic_get(&p->refcnt) == 0);
	if (p->service != NULL) {
		struct weenet_process *running = _running_process();
		_set_running_process(p);
		weenet_service_delete(p->service, p);
		_set_running_process(running);
	}
	weenet_monitor_unlink(&p->supervisees, p);
	weenet_mailbox_cleanup(&p->mailbox);
	_process_delete(p);
}

void
weenet_process_retire(struct weenet_process *p) {
	if (weenet_atomic_cas(&p->retired, false, true)) {
		weenet_process_push(p, 0, 0, WMESSAGE_TYPE_RETIRED | WMESSAGE_FLAG_INTERNAL, 0, 0);
	}
}

struct weenet_process *
weenet_process_retain(struct weenet_process *p) {
	weenet_atomic_add(&p->refcnt, 1);
	return p;
}

// Called by unregister
bool
weenet_process_release(struct weenet_process *p) {
	int32_t ref = weenet_atomic_sub(&p->refcnt, 1);
	assert(ref >= 0);
	if (ref == 0) {
		if (!p->retired) {
			weenet_logger_fatalf("process[%ld name(%s)] unexpected terminated.\n",
				(long)p->id, p->name);
		}
		weenet_process_delete(p);
		return true;
	}
	return false;
}

session_t
weenet_process_sid(struct weenet_process *p) {
	session_t sid = ++p->session;
	if (sid == SESSION_ZERO) {
		sid = ++p->session;
	}
	return sid;
}

process_t
weenet_process_pid(const struct weenet_process *p) {
	return _pid(p);
}

const char *
weenet_process_name(const struct weenet_process *p) {
	return _name(p);
}

struct weenet_process *
weenet_process_self() {
	return _running_process();
}

static bool
weenet_process_work(struct weenet_process *p) {
	struct weenet_message *msg = weenet_mailbox_pop(&p->mailbox);
	if (msg == NULL) return false;

	if ((msg->tags & WMESSAGE_FLAG_INTERNAL)) {
		msg->tags &= ~(uint32_t)WMESSAGE_FLAG_INTERNAL;
		uint32_t type = weenet_message_type(msg);
		switch (type) {
		case WMESSAGE_TYPE_MONITOR:
			;monitor_t mref = (monitor_t)msg->meta;
			if (msg->data == 0) {
				weenet_monitor_remove(&p->supervisors, mref, (uintptr_t)msg->source);
			} else if (weenet_atomic_get(&p->retired) == true) {
				weenet_process_retain(p);
				weenet_process_send(msg->source, _pid(p), 0, WMESSAGE_TAGS_RETIRED, (uintptr_t)p, mref);
			} else {
				weenet_monitor_insert(&p->supervisors, mref, (uintptr_t)msg->source);
			}
			break;
		case WMESSAGE_TYPE_RETIRED:
			if (msg->source == 0) {	// send by weenet_process_retire()
				// 'p' is retired, no more lock need.
				assert(p->retired == true);
				// Send retired message to all processes that monitoring 'p'
				weenet_monitor_notify(&p->supervisors, p);
				// XXX A dedicated 'RETIRED' service to terminate retired processes ?
				weenet_process_release(p);
			} else if (weenet_monitor_remove(&p->supervisees, (monitor_t)msg->meta, (uintptr_t)msg->data)) {
				// Filter out cancelled monitoring.
				weenet_service_handle(p->service, p, msg);
				weenet_process_release((struct weenet_process *)msg->data);
			}
			break;
		default:
			weenet_logger_fatalf("unexpected internal message type[%d]", type);
			break;
		}
		weenet_message_unref(msg);
		return true;
	}

	weenet_service_handle(p->service, p, msg);
	weenet_message_unref(msg);
	return p->wait.session == SESSION_ZERO;
}

void
weenet_process_wait(struct weenet_process *p, session_t sid) {
	p->wait.session = sid;
}

// XXX Exported API should check invalid WMESSAGE_FLAG_INTERNAL message.
void
weenet_process_mail(struct weenet_process *p, struct weenet_message *m) {
	if (p->wait.session != 0 && p->wait.session == m->session && (m->tags & WMESSAGE_FLAG_RESPONSE)) {
		p->wait.session = 0;
		weenet_mailbox_insert(&p->mailbox, m);
		weenet_schedule_resume(p);
	} else if (weenet_mailbox_push(&p->mailbox, m)) {
		weenet_schedule_resume(p);
	}
}

void
weenet_process_push(struct weenet_process *p, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_message *m = weenet_message_new(src, sid, tags, data, meta);
	weenet_process_mail(p, m);
}

session_t
weenet_process_cast(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_process *p1 = weenet_account_retain(dst);
	if (p1 == NULL) {
		_reclaim_resource(tags, data, meta);
		return 0;
	}
	session_t sid = weenet_process_sid(p);
	process_t src = weenet_process_pid(p);
	weenet_process_push(p1, src, sid, tags, data, meta);
	weenet_process_release(p1);
	return sid;
}

session_t
weenet_process_call(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_process *out = weenet_account_retain(dst);
	if (out == NULL) {
		_reclaim_resource(tags, data, meta);
		return 0;
	}
	session_t sid = weenet_process_sid(p);
	weenet_process_wait(p, sid);
	process_t src = weenet_process_pid(p);
	weenet_process_push(out, src, sid, tags, data, meta);
	weenet_process_release(out);
	return sid;
}

int
weenet_process_send(process_t dst, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_process *out = weenet_account_retain(dst);
	if (out == NULL) {
		_reclaim_resource(tags, data, meta);
		return -1;
	}
	weenet_process_push(out, src, sid, tags, data, meta);
	weenet_process_release(out);
	return 0;
}

bool
weenet_process_forward(process_t dst, struct weenet_message *m) {
	struct weenet_process *p = weenet_account_retain(dst);
	if (p == NULL) {
		return false;
	}
	weenet_message_ref(m);
	weenet_process_mail(p, m);
	weenet_process_release(p);
	return true;
}

session_t
weenet_process_timeo(struct weenet_process *p, uint64_t msecs) {
	session_t pid = weenet_process_pid(p);
	session_t sid = weenet_process_sid(p);
	weenet_time_timeout(pid, sid, msecs);
	return sid;
}

bool
weenet_process_resume(struct weenet_process *p) {
	assert(_running_process() == NULL);
	_set_running_process(p);
	for (int i=0; i<10; ++i) {
		if (!weenet_process_work(p)) {
			_set_running_process(NULL);
			return false;
		}
	}
	_set_running_process(NULL);
	return true;
}

#define _ref(p)		weenet_process_retain(p)

monitor_t
weenet_process_monitor(struct weenet_process *p) {
	struct weenet_process *self = _running_process();
	monitor_t mref = ++self->mref;
	weenet_monitor_insert(&self->supervisees, mref, (uintptr_t)_ref(p));

	bool retired = weenet_atomic_get(&p->retired);
	if (retired) {
		weenet_process_push(self, _pid(p), 0, WMESSAGE_TAGS_RETIRED, (uintptr_t)_ref(p), (uintptr_t)mref);
	} else {
		weenet_process_push(p, _pid(self), 0, WMESSAGE_TAGS_MONITOR, 1, (uintptr_t)mref);
	}
	return mref;
}

void
weenet_process_demonitor(monitor_t mref) {
	struct weenet_process *self = _running_process();
	struct weenet_process *p = weenet_monitor_erase(&self->supervisees, mref);
	if (p != NULL) {
		weenet_process_push(p, _pid(self), 0, WMESSAGE_TAGS_MONITOR, 0, mref);
		weenet_process_release(p);
	}
}

int
weenet_init_process() {
	process_slab = slab_new(1024, sizeof(struct weenet_process));
	message_slab = slab_new(10240, sizeof(struct weenet_message));
	weenet_message_gc(WMESSAGE_RIDX_FILE, NULL, _file_resource_release);
	weenet_message_gc(WMESSAGE_RIDX_PROC, NULL, _process_resource_release);
	weenet_message_gc(WMESSAGE_RIDX_RAWMEM, NULL, _rawmem_resource_release);
	weenet_message_gc(WMESSAGE_RIDX_MEMORY, NULL, _memory_resource_release);
	T = wcalloc(sizeof(*T));
	T->free.first = -1;
	T->size = 65536;
	T->slot = wmalloc(T->size * sizeof(T->slot[0]));
	T->slot[0].proc = NULL;
	return 0;
}
