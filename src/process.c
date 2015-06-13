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

enum wmessage_flag {
	WMSG_FLAG_INTERNAL		= 0x01,
	WMSG_FLAG_MIGRATED		= 0x02,	// resource ownership migrated to other
};

static const union message_tags WMSG_TAGS_MONITOR	= {.code = WMSG_CODE_MONITOR, .flag = WMSG_FLAG_INTERNAL};
static const union message_tags WMSG_TAGS_RETIRED	= {.ridx = WMSG_RIDX_PROC, .code = WMSG_CODE_RETIRED, .flag = WMSG_FLAG_INTERNAL};

struct message_queue {
	struct weenet_message *first;
	struct weenet_message **last;
};

inline static void
_message_queue_init(struct message_queue *inbox) {
	inbox->last = &inbox->first;
}

inline static bool
_message_queue_empty(struct message_queue *inbox) {
	return inbox->last == &inbox->first;
}

inline static void
_message_queue_push(struct message_queue *inbox, struct weenet_message *m) {
	*inbox->last = m;
	inbox->last = &m->next;
}

inline static struct weenet_message *
_message_queue_clear(struct message_queue *inbox) {
	*inbox->last = NULL;
	struct weenet_message *msgs = inbox->first;
	_message_queue_init(inbox);
	return msgs;
}

struct weenet_mailbox {
	int32_t size;
	int32_t lock;
	int32_t active;
	struct weenet_message *msgs;
	struct message_queue inbox;
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
	} pairs[WMSG_RIDX_MASK+1];
} F;

static void
_free_resource(uint32_t ridx, uintptr_t data, uintptr_t meta) {
	resource_fini_t fn = F.pairs[ridx].fn;
	if (fn != NULL) {
		fn(F.pairs[ridx].ud, data, meta);
	}
}

static void
_reclaim_resource(uint32_t tags, uintptr_t data, uintptr_t meta) {
	union message_tags t = {.value = tags};
	if ((t.flag & WMSG_FLAG_MIGRATED)) {
		return;
	}
	_free_resource(t.ridx, data, meta);
}

int
weenet_message_gc(uint8_t id, void *ud, resource_fini_t fn) {
	if (id == 0) return EINVAL;
	if (id > WMSG_RIDX_MASK) return ERANGE;
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
	msg->tags.value = tags;
	msg->refcnt = 1;
	return msg;
}

void
weenet_message_delete(struct weenet_message *m) {
	_reclaim_resource(m->tags.value, m->data, m->meta);
	slab_release(message_slab, m);
}

// XXX Multicast message may exist race condition ?
void
weenet_message_take(struct weenet_message *msg) {
	msg->tags.flag |= WMSG_FLAG_MIGRATED;
}

struct weenet_message *
weenet_message_ref(struct weenet_message *msg) {
	weenet_atomic_inc(&msg->refcnt);
	return msg;
}

void
weenet_message_copy(struct weenet_message *msg, unsigned n) {
	weenet_atomic_add(&msg->refcnt, (int32_t)n);
}

void
weenet_message_unref(struct weenet_message *msg) {
	int32_t refcnt = weenet_atomic_sub(&msg->refcnt, 1);
	if (refcnt == 0) {
		weenet_message_delete(msg);
	}
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
			weenet_process_send(pid, self, 0, WMSG_TAGS_RETIRED.value, (uintptr_t)p, monitors[i].mref);
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
		weenet_process_push(dst, _pid(p), 0, WMSG_TAGS_MONITOR.value, 0, (uintptr_t)m->monitors[i].mref);
		weenet_process_release(dst);
	}
	wfree(m->monitors);
	m->monitors = NULL;
	m->num = m->len = 0;
}

inline static void
weenet_mailbox_init(struct weenet_mailbox *b) {
	b->active = true;
	_message_queue_init(&b->inbox);
}

static struct weenet_message *
weenet_mailbox_take(struct weenet_mailbox *b) {
	assert(b->active);
	if (b->msgs == NULL) {
		weenet_atomic_lock(&b->lock);
		if (_message_queue_empty(&b->inbox)) {
			b->active = false;
			weenet_atomic_unlock(&b->lock);
			return NULL;
		}
		struct weenet_message *msgs = _message_queue_clear(&b->inbox);
		weenet_atomic_unlock(&b->lock);
		b->msgs = msgs;
	}
	struct weenet_message *m = b->msgs;
	b->msgs = m->next;
	m->next = NULL;
	return m;
}

static bool
weenet_mailbox_push(struct weenet_mailbox *b, struct weenet_message *m) {
	weenet_atomic_lock(&b->lock);
	_message_queue_push(&b->inbox, m);
	weenet_atomic_unlock(&b->lock);
	weenet_atomic_inc(&b->size);
	return weenet_atomic_cas(&b->active, false, true);
}

static void
weenet_mailbox_insert(struct weenet_mailbox *b, struct weenet_message *m) {
	assert(b->active);		// Must be active. There is no reader.
	for (;;) {
		struct weenet_message *head = b->msgs;
		m->next = head;
		if (weenet_atomic_cas(&b->msgs, head, m)) {
			break;
		}
	}
	weenet_atomic_inc(&b->size);
}

static void
weenet_mailbox_cleanup(struct weenet_mailbox *b) {
	struct weenet_message *msg_groups[2];
	msg_groups[0] = b->msgs;
	msg_groups[1] = _message_queue_clear(&b->inbox);
	for (int i=0; i<2; ++i) {
		struct weenet_message *msgs = msg_groups[0];
		while (msgs != NULL) {
			struct weenet_message *m = msgs;
			msgs = m->next;
			weenet_message_unref(m);
		}
	}
}

struct weenet_registry {
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

static struct weenet_registry *REGISTRY;

static process_t
weenet_process_link(struct weenet_process *p) {
	++p->refcnt;
	struct weenet_registry *registry = REGISTRY;
	weenet_atomic_lock(&registry->lock);
	if (registry->len == registry->size) {
		intreg_t index = registry->free.first;
		if (index != -1) {
			assert(registry->slot[index].proc == NULL);
			registry->free.first = registry->slot[index].next;
			registry->slot[index].proc = p;
			weenet_atomic_unlock(&registry->lock);
			return (process_t)index;
		} else {
			size_t size = registry->size;
			size += size/2+1;
			registry->slot = wrealloc(registry->slot, size * sizeof(registry->slot[0]));
			registry->size = size;
		}
	}
	assert(registry->len < registry->size);
	size_t index = ++registry->len;
	registry->slot[index].proc = p;
	weenet_atomic_unlock(&registry->lock);
	return (process_t)index;
}

static void
weenet_process_unlink(process_t pid) {
	if (pid == PROCESS_ZERO) return;
	struct weenet_registry *registry = REGISTRY;
	weenet_atomic_lock(&registry->lock);
	intreg_t index = (intreg_t)pid;
	assert((size_t)index <= registry->len);
	if (registry->slot[index].proc == NULL) {
		weenet_atomic_unlock(&registry->lock);
		return;
	}
	registry->slot[index].proc = NULL;
	registry->slot[index].next = -1;
	if (registry->free.first == -1) {
		registry->free.first = registry->free.last = index;
	} else {
		assert(registry->slot[registry->free.last].next == -1);
		registry->slot[registry->free.last].next = index;
		registry->free.last = index;
	}
	weenet_atomic_unlock(&registry->lock);
}

struct weenet_process *
weenet_process_find(process_t pid) {
	struct weenet_registry *registry = REGISTRY;
	weenet_atomic_lock(&registry->lock);
	assert((size_t)pid <= registry->len);
	struct weenet_process *p = registry->slot[pid].proc;
	if (p != NULL) {
		if (p->refcnt == 0) {
			weenet_atomic_unlock(&registry->lock);
			return NULL;
		}
		// retained under lock
		int32_t refcnt = weenet_atomic_add(&p->refcnt, 1);
		if (refcnt == 1) {
			// Someone just released it, about to delete it.
			weenet_atomic_sub(&p->refcnt, 1);
			weenet_atomic_unlock(&registry->lock);
			return NULL;
		}
	}
	weenet_atomic_unlock(&registry->lock);
	return p;
}

void
weenet_process_kill(process_t pid) {
	struct weenet_process *p = weenet_process_find(pid);
	if (p == NULL) return;
	weenet_process_retire(p);
	weenet_process_release(p);
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
	p->id = weenet_process_link(p);
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
	weenet_process_unlink(p->id);
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
		union message_tags tags = { .code = WMSG_CODE_RETIRED, .flag = WMSG_FLAG_INTERNAL };
		weenet_process_push(p, 0, 0, tags.value, 0, 0);
	}
}

void
weenet_process_suicide() {
	struct weenet_process *self = _running_process();
	weenet_process_retire(self);
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
weenet_process_running() {
	return _running_process();
}

process_t
weenet_process_self() {
	struct weenet_process *p = weenet_process_running();
	if (p) {
		return weenet_process_pid(p);
	}
	return 0;
}

static bool
weenet_process_work(struct weenet_process *p) {
	struct weenet_message *msg = weenet_mailbox_take(&p->mailbox);
	if (msg == NULL) return false;

	if ((msg->tags.flag & WMSG_FLAG_INTERNAL)) {
		// msg->tags.flag &= ~(uint32_t)WMSG_FLAG_INTERNAL;
		uint32_t code = weenet_message_code(msg);
		switch (code) {
		case WMSG_CODE_MONITOR:
			;monitor_t mref = (monitor_t)msg->meta;
			if (msg->data == 0) {
				weenet_monitor_remove(&p->supervisors, mref, (uintptr_t)msg->source);
			} else if (weenet_atomic_get(&p->retired) == true) {
				weenet_process_retain(p);
				weenet_process_send(msg->source, _pid(p), 0, WMSG_TAGS_RETIRED.value, (uintptr_t)p, mref);
			} else {
				weenet_monitor_insert(&p->supervisors, mref, (uintptr_t)msg->source);
			}
			break;
		case WMSG_CODE_RETIRED:
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
			weenet_logger_fatalf("unexpected internal message code[%d]", code);
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

// XXX Exported API should check invalid WMSG_FLAG_INTERNAL message.
void
weenet_process_mail(struct weenet_process *p, struct weenet_message *m) {
	if (p->wait.session != 0 && p->wait.session == m->session && m->tags.kind == WMSG_KIND_REQUEST) {
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
	struct weenet_process *p1 = weenet_process_find(dst);
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
	struct weenet_process *out = weenet_process_find(dst);
	if (out == NULL) {
		_reclaim_resource(tags, data, meta);
		return 0;
	}
	session_t sid = weenet_process_sid(p);
	weenet_process_wait(p, sid);
	process_t src = weenet_process_pid(p);
	union message_tags t = { .value = tags };
	t.kind = WMSG_KIND_REQUEST;
	weenet_process_push(out, src, sid, t.value, data, meta);
	weenet_process_release(out);
	return sid;
}

bool
weenet_process_send(process_t dst, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_process *out = weenet_process_find(dst);
	if (out == NULL) {
		_reclaim_resource(tags, data, meta);
		return false;
	}
	weenet_process_push(out, src, sid, tags, data, meta);
	weenet_process_release(out);
	return true;
}

session_t
weenet_process_request(process_t pid, uint32_t ridx, uint32_t code, uintptr_t data, uintptr_t meta) {
	struct weenet_process *out = weenet_process_find(pid);
	if (out == NULL) {
		_free_resource(ridx, data, meta);
		return 0;
	}
	struct weenet_process *self = weenet_process_running();
	assert(self != NULL);
	process_t source = weenet_process_pid(self);
	session_t session = weenet_process_sid(self);
	uint32_t tags = weenet_combine_tags(ridx, WMSG_KIND_REQUEST, code);
	weenet_process_push(out, source, session, tags, data, meta);
	weenet_process_release(out);
	return session;
}

bool
weenet_process_notify(process_t pid, uint32_t ridx, uint32_t code, uintptr_t data, uintptr_t meta) {
	uint32_t tags = weenet_combine_tags(ridx, WMSG_KIND_NOTIFY, code);
	process_t self = weenet_process_self();
	return weenet_process_send(pid, self, 0, tags, data, meta);
}

bool
weenet_process_response(process_t pid, session_t session, uint32_t ridx, uint32_t code, uintptr_t data, uintptr_t meta) {
	uint32_t tags = weenet_combine_tags(ridx, WMSG_KIND_RESPONSE, code);
	process_t self = weenet_process_self();
	return weenet_process_send(pid, self, session, tags, data, meta);
}

bool
weenet_process_forward(process_t dst, struct weenet_message *m) {
	struct weenet_process *p = weenet_process_find(dst);
	if (p == NULL) {
		return false;
	}
	weenet_message_ref(m);
	weenet_process_mail(p, m);
	weenet_process_release(p);
	return true;
}

session_t
weenet_process_timeout(uint64_t msecs, uintreg_t flag) {
	struct weenet_process *p = weenet_process_running();
	process_t pid = weenet_process_pid(p);
	session_t session = (flag == WMSG_KIND_REQUEST) ? weenet_process_sid(p) : 0;
	weenet_timeout(pid, session, msecs);
	return session;
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
		weenet_process_push(self, _pid(p), 0, WMSG_TAGS_RETIRED.value, (uintptr_t)_ref(p), (uintptr_t)mref);
	} else {
		weenet_process_push(p, _pid(self), 0, WMSG_TAGS_MONITOR.value, 1, (uintptr_t)mref);
	}
	return mref;
}

void
weenet_process_demonitor(monitor_t mref) {
	struct weenet_process *self = _running_process();
	struct weenet_process *p = weenet_monitor_erase(&self->supervisees, mref);
	if (p != NULL) {
		weenet_process_push(p, _pid(self), 0, WMSG_TAGS_MONITOR.value, 0, mref);
		weenet_process_release(p);
	}
}

int
weenet_init_process() {
	process_slab = slab_new(1024, sizeof(struct weenet_process));
	message_slab = slab_new(10240, sizeof(struct weenet_message));
	weenet_message_gc(WMSG_RIDX_FILE, NULL, _file_resource_release);
	weenet_message_gc(WMSG_RIDX_PROC, NULL, _process_resource_release);
	weenet_message_gc(WMSG_RIDX_RAWMEM, NULL, _rawmem_resource_release);
	weenet_message_gc(WMSG_RIDX_MEMORY, NULL, _memory_resource_release);
	struct weenet_registry *registry = REGISTRY = wcalloc(sizeof(*REGISTRY));
	registry->free.first = -1;
	registry->size = 65536;
	registry->slot = wmalloc(registry->size * sizeof(registry->slot[0]));
	registry->slot[0].proc = NULL;
	return 0;
}
