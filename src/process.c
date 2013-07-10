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
	struct {
		uintreg_t mref;
		struct weenet_process *proc;
	} *monitors;
};

struct weenet_process {
	process_t id;
	session_t session;
	struct {
		process_t source;
		session_t session;
	} wait;
	struct weenet_atom *name;
	struct weenet_service *service;
	struct weenet_mailbox mailbox;

	uint32_t mref;		// monitor reference, just/almost unique in this process.
	int32_t refcnt;
	int lock;	// protect 'retired' 'supervisors'
	bool retired;	// integer ?
	struct weenet_monitor supervisors;	// processes that monitoring this process
	struct weenet_monitor supervisees;	// processes that this process monitoring
};

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

struct weenet_process *
weenet_process_calloc() {
	struct weenet_process *p = slab_retain(process_slab);
	memzero(p, sizeof(*p));
	return p;
}

void
weenet_process_free(struct weenet_process *p) {
	slab_release(process_slab, p);
}

static struct {
	struct {
		void *ud;
		resource_fini_t fn;
	} pairs[WMESSAGE_RIDX_MASK+1];
} F;

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
	if ((m->tags & WMESSAGE_FLAG_MIGRATED) == 0) {
		uint32_t idx = weenet_message_ridx(m);
		resource_fini_t fn = F.pairs[idx].fn;
		if (fn != NULL) {
			fn(F.pairs[idx].ud, m->data, m->meta);
		}
	}
	slab_release(message_slab, m);
}

// XXX Multicast message may exist race condition ?
void
weenet_message_take(struct weenet_message *msg) {
	msg->tags |= WMESSAGE_FLAG_MIGRATED;
}

void
weenet_message_ref(struct weenet_message *msg) {
	weenet_atomic_inc(&msg->refcnt);
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

static void
weenet_monitor_retire(struct weenet_monitor *m, struct weenet_process *p) {
	uint32_t n = m->num;
	if (n != 0) {
		weenet_atomic_add(&p->refcnt, (int32_t)n);
		process_t self = p->id;
		for (uint32_t i=0; i<n; ++i) {
			weenet_process_push(m->monitors[i].proc, self, 0, WMESSAGE_TAGS_RETIRED, (uintptr_t)p, m->monitors[i].mref);
		}
	}
	wfree(m->monitors);
	m->monitors = NULL;
	m->num = m->len = 0;
}

static void
weenet_monitor_insert(struct weenet_monitor *m, uintreg_t mref, struct weenet_process *proc) {
	uint32_t n = m->num++;
	if (n == m->len) {
		m->len = 2*n + 1;
		m->monitors = wrealloc(m->monitors, m->len * sizeof(m->monitors[0]));
	}
	m->monitors[n].mref = mref;
	m->monitors[n].proc = proc;
}

static struct weenet_process *
weenet_monitor_remove(struct weenet_monitor *m, uintreg_t mref, struct weenet_process *proc) {
	uint32_t n = m->num;
	if (n == 0) return false;
	uint32_t last = n-1;
	if (proc == NULL) {
		// For supervisor, mref is ok to demonitor.
		for (uint32_t i=0; i<=last; ++i) {
			if (m->monitors[i].mref == mref) {
				struct weenet_process *dst = m->monitors[i].proc;
				if (i != last) {
					m->monitors[i].mref = m->monitors[last].mref;
					m->monitors[i].proc = m->monitors[last].proc;
				}
				m->num = last;
				return dst;
			}
		}
	} else {
		for (uint32_t i=0; i<=last; ++i) {
			if (m->monitors[i].mref == mref && m->monitors[i].proc == proc) {
				if (i != last) {
					m->monitors[i].mref = m->monitors[last].mref;
					m->monitors[i].proc = m->monitors[last].proc;
				}
				m->num = last;
				return NULL;
			}
		}
	}
	return NULL;
}

static void
_demonitor(struct weenet_process *p, monitor_t mref, struct weenet_process *src) {
	weenet_atomic_lock(&p->lock);
	if (!p->retired) {
		weenet_monitor_remove(&p->supervisors, mref, src);
	}	// else flush message, do it in weenet_process_work().
	weenet_atomic_unlock(&p->lock);
}

static void
weenet_monitor_unlink(struct weenet_monitor *m, struct weenet_process *p) {
	uint32_t n = m->num;
	for (uint32_t i=0; i<n; ++i) {
		struct weenet_process *dst = m->monitors[i].proc;
		monitor_t mref = m->monitors[i].mref;
		_demonitor(dst, mref, p);
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

inline static int32_t
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
		intptr_t first;
		intptr_t last;
	} free;
	struct weenet_process **processes;
};

void weenet_process_retire(struct weenet_process *p);

static struct weenet_account *T;

process_t
weenet_account_enroll(struct weenet_process *p) {
	++p->refcnt;
	struct weenet_account *t = T;
	weenet_atomic_lock(&t->lock);
	if (t->len == t->size) {
		if (t->free.first != 0) {
			process_t pid = (process_t)t->free.first;
			t->free.first = (intptr_t)t->processes[pid];
			--t->free.number;
			t->processes[pid] = p;
			weenet_atomic_unlock(&t->lock);
			return pid;
		} else {
			size_t size = t->size;
			size += size/2;
			t->processes = wrealloc(t->processes, size);
			t->size = size;
		}
	}
	assert(t->len < t->size);
	process_t pid = (process_t)t->len++;
	t->processes[pid] = p;
	weenet_atomic_unlock(&t->lock);
	return pid;
}

void
weenet_account_unlink(process_t pid) {
	if (pid == PROCESS_ZERO) return;
	struct weenet_account *t = T;
	weenet_atomic_lock(&t->lock);
	assert((size_t)pid < t->len);
	if (t->processes[pid] == NULL) {
		weenet_atomic_unlock(&t->lock);
		return;
	}
	// FIXME another storage to store free-list info.
	t->free.last = (intptr_t)pid;
	if (t->free.first == 0) {
		t->free.first = t->free.last = (intptr_t)pid;
	} else {
		assert((intptr_t)t->processes[t->free.last] == 0);
		t->free.last = (intptr_t)pid;
	}
	++t->free.number;
	t->processes[pid] = (void *)(intptr_t)(0);
	weenet_atomic_unlock(&t->lock);
}

struct weenet_process *
weenet_account_retain(process_t pid) {
	struct weenet_account *t = T;
	weenet_atomic_lock(&t->lock);
	struct weenet_process *p = t->processes[pid];
	if (p != NULL) {
		// retained under lock
		weenet_process_retain(p);
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

struct weenet_process *
weenet_process_new(const char *name, uintptr_t data, uintptr_t meta) {
	struct weenet_process *p = weenet_process_calloc();
	p->refcnt = 1;	// Two? One for new, one for all monitors?
	p->id = weenet_account_enroll(p);
	p->name = weenet_atom_new(name, strlen(name));
	p->service = weenet_service_new(p->name, p, data, meta);
	if (p->service == NULL) {
		fprintf(stderr, "failed to start new process [%s].\n", name);
		weenet_process_release(p);
		weenet_process_retire(p);
		return NULL;
	}
	return p;
}

static void
weenet_process_delete(struct weenet_process *p) {
	assert(weenet_atomic_get(&p->refcnt) == 0);
	if (p->service != NULL) {
		weenet_service_delete(p->service, p);
	}
	weenet_monitor_unlink(&p->supervisees, p);
	weenet_mailbox_cleanup(&p->mailbox);
	weenet_process_free(p);
}

void
weenet_process_retire(struct weenet_process *p) {
	bool send = false;
	weenet_atomic_lock(&p->lock);
	if (!p->retired) {
		send = p->retired = true;
	}
	weenet_atomic_unlock(&p->lock);
	if (send) {
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
				(long)p->id, weenet_atom_str(p->name));
		}
		weenet_process_delete(p);
		weenet_account_unlink(p->id);
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
weenet_process_self(const struct weenet_process *p) {
	return p->id;
}

session_t
weenet_process_boot(struct weenet_process *p, uint32_t tags, uintptr_t data, uintptr_t meta) {
	assert(weenet_mailbox_num(&p->mailbox) == 0);
	session_t sid = weenet_process_sid(p);
	weenet_process_push(p, 0, sid, tags | WMESSAGE_TYPE_BOOT, data, meta);
	return sid;
}

static bool
weenet_process_work(struct weenet_process *p) {
	struct weenet_message *msg = weenet_mailbox_pop(&p->mailbox);
	if (msg == NULL) return false;

	if ((msg->tags & WMESSAGE_FLAG_INTERNAL)) {
		msg->tags &= ~WMESSAGE_FLAG_INTERNAL;
		uint32_t type = weenet_message_type(msg);
		if (type == WMESSAGE_TYPE_RETIRED) {
			struct weenet_process *dst = (struct weenet_process *)msg->data;
			if (dst == NULL) {	// send by weenet_process_retire()
				// 'p' is retired, no more lock need.
				assert(p->retired == true);
				// Send retired message to all processes that monitoring 'p'
				weenet_monitor_retire(&p->supervisors, p);
				// XXX A dedicated 'RETIRED' service to terminate retired processes ?
				weenet_process_release(p);
			} else if (weenet_monitor_remove(&p->supervisees, (uintreg_t)msg->meta, NULL) != NULL) {
				// Filter out cancelled monitoring.
				weenet_service_handle(p->service, p, msg);
			}
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

// Can't wakeup blocked process.
void
weenet_process_wakeup(struct weenet_process *p) {
	weenet_process_push(p, 0, 0, WMESSAGE_TYPE_WAKEUP, 0, 0);
}

session_t
weenet_process_cast(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_process *p1 = weenet_account_retain(dst);
	if (p1 == NULL) {
		return 0;
	}
	session_t sid = weenet_process_sid(p);
	process_t src = weenet_process_self(p);
	weenet_process_push(p1, src, sid, tags, data, meta);
	weenet_process_release(p1);
	return sid;
}

session_t
weenet_process_call(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_process *out = weenet_account_retain(dst);
	if (out == NULL) {
		return 0;
	}
	session_t sid = weenet_process_sid(p);
	weenet_process_wait(p, sid);
	process_t src = weenet_process_self(p);
	weenet_process_push(out, src, sid, tags, data, meta);
	weenet_process_release(out);
	return sid;
}

int
weenet_process_send(process_t dst, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta) {
	struct weenet_process *out = weenet_account_retain(dst);
	if (out == NULL) {
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
	session_t pid = weenet_process_self(p);
	session_t sid = weenet_process_sid(p);
	weenet_timer_timeout(pid, sid, msecs);
	return sid;
}

bool
weenet_process_resume(struct weenet_process *p) {
	for (int i=0; i<10; ++i) {
		if (!weenet_process_work(p)) {
			return false;
		}
	}
	return true;
}

uintreg_t
weenet_process_monitor(struct weenet_process *p, struct weenet_process *dst) {
	uint64_t mref = weenet_atomic_inc(&p->mref);
	bool retired = false;
	weenet_atomic_lock(&dst->lock);
	retired = dst->retired;
	if (!retired) {
		// XXX take a reference ?
		// Semantics of this API requires retained 'dst',
		// but the implementation seems not.
		weenet_monitor_insert(&dst->supervisors, mref, p);
	}
	weenet_atomic_unlock(&dst->lock);
	weenet_monitor_insert(&p->supervisees, mref, dst);
	if (retired) {
		weenet_process_push(p, dst->id, 0, WMESSAGE_TAGS_RETIRED, (uintptr_t)weenet_process_retain(dst), (uintptr_t)mref);
	}
	return mref;
}

void
weenet_process_demonitor(struct weenet_process *p, uintreg_t mref) {
	struct weenet_process *dst = weenet_monitor_remove(&p->supervisees, mref, NULL);
	if (dst != NULL) {
		_demonitor(dst, mref, p);
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
	T = wmalloc(sizeof(*T) + 65535*sizeof(void*));
	T->size = 65535;
	T->processes = (void*)(T+1);
	return 0;
}
