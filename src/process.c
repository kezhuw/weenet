#include "process.h"

#include "atom.h"
#include "atomic.h"
#include "logger.h"
#include "memory.h"
#include "service.h"
#include "schedule.h"
#include "slab.h"
#include "timer.h"

#include <errno.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define memzero(ptr, size)	memset(ptr, 0, size)

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
	struct weenet_process **procs;
};

struct weenet_process {
	process_t id;
	session_t session;
	struct {
		process_t source;
		session_t session;
	} wait;
	int64_t refcnt;
	bool retired;
	struct weenet_service *service;
	struct weenet_mailbox mailbox;
	const char *name;	// atom name
};

static /*__thread*/ struct slab *process_slab;
static struct slab *message_slab;

//struct process_config {
//};
//
//static uint64_t process_mem_granularity = 1000;
//static uint64_t message_mem_granularity = 10000;
//
int
weenet_bootstrap_process() {
	process_slab = slab_new(1024, sizeof(struct weenet_process));
	message_slab = slab_new(10240, sizeof(struct weenet_message));
	return 0;
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

#define WMESSAGE_FINI_SIZE	64
#define WMESSAGE_FINI_MASK	(WMESSAGE_FINI_SIZE-1)

static struct {
	struct {
		void *ud;
		void (*fn)(void *ud, uintptr_t data, uintptr_t meta);
	} finis[WMESSAGE_FINI_SIZE];
} F;

static uint32_t
tags_to_fini_seq(uint32_t tags) {
	// TODO
	(void)tags;
	return 0;
}

//enum {
//	WFINALIZER_SET,
//	WFINALIZER_RESET,
//	WFINALIZER_CLEAR,
//};
//
int
weenet_message_gc(uint32_t id, void *ud, void (*fini)(void *ud, uintptr_t data, uintptr_t meta)) {
	if (id == 0) return EINVAL;
	if (id >= WMESSAGE_FINI_SIZE) return ERANGE;
	if (F.finis[id].fn != NULL) return EEXIST;
	F.finis[id].ud = ud;
	F.finis[id].fn = fini;
	weenet_atomic_sync();
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
	uint32_t seq = tags_to_fini_seq(m->tags);
	void (*fn)(void *ud, uintptr_t data, uintptr_t meta) = F.finis[seq].fn;
	if (fn != NULL) {
		void *ud = F.finis[seq].ud;
		fn(ud, m->data, m->meta);
	}
	slab_release(message_slab, m);
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
weenet_mailbox_expand(struct weenet_mailbox *b) {
	assert(weenet_atomic_locked(&b->lock));
	assert(b->num == b->size);
	uint32_t size = b->size;
	uint32_t newsize = 2*size + 1;
	b->mbox = wrealloc(b->mbox, sizeof(void*) * newsize);
	// Mailbox is full, two situations:
	//
	//   1) head(0) ------ rear(size)
	//   2) ----- head(rear) --------
	uint32_t rear = b->rear;
	if (rear != size) {
		// Situation 2.
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

//struct weenet_message *
//weenet_mailbox_pop(struct weenet_mailbox *mbox) {
//	struct mail *mail;
//	assert(mbox->active == 1);	// The only reader.
//	lock(&mbox->lock);
//	mail = mbox->first;
//	if (mail != NULL) {
//		mbox->first = mail->link;
//		unlock(&mbox->lock);
//		__sync_sub_and_fetch(&mbox->number, 1);	// bug: without lock, may reduce to < 0.
//		return &mail->body;
//	}
//	mbox->active = 0;
//	unlock(&mbox->lock);
//	return NULL;
//}
//
//// True, indicates that reader is in sleeping; Caller need to wake it up.
//bool
//weenet_mailbox_push(struct weenet_mailbox *mbox, struct weenet_message *msg) {
//	struct mail *mail = (void*)((char*)msg - offsetof(struct mail, body));
//	mail->link = NULL;
//	bool sleeping = false;
//	lock(&mbox->lock);
//	if (mbox->first == NULL) {
//		mbox->first = mail;
//		mbox->last = mail;
//		if (mbox->active == 0) {
//			mbox->active = 1;
//			sleeping = true;
//		}
//	} else {
//		mbox->last->link = mail;
//		mbox->last = mail;
//		assert(mbox->active == 1);
//	}
//	unlock(&mbox->lock);
//	__sync_add_and_fetch(&mbox->number, 1);
//	return sleeping;
//}
//
//void
//weenet_mailbox_insert(struct weenet_mailbox *mbox, struct weenet_message *msg) {
//	assert(mbox->active == 1);	// There is no reader.
//	struct mail *mail = (void*)((char*)msg - offsetof(struct mail, body));
//	lock(&mbox->lock);
//	struct mail *first = mbox->first;
//	mail->link = first;
//	mbox->first = mail;
//	if (first == NULL) {
//		mbox->last = mail;
//	}
//	unlock(&mbox->lock);
//	__sync_add_and_fetch(&mbox->number, 1);
//}

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

bool weenet_process_retire(struct weenet_process *p);

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

// XXX store 'retired' in account ?
bool
weenet_account_retire(process_t pid) {
	struct weenet_process *p = weenet_account_retain(pid);
	if (p == NULL) return true;
	weenet_process_retire(p);
	return weenet_process_release(p);
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
	p->refcnt = 1;
	p->id = weenet_account_enroll(p);
	p->name = weenet_atom_str(weenet_atom_new(name, strlen(name)));
	p->service = weenet_service_new(name, p, data, meta);
	if (p->service == NULL) {
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
	weenet_mailbox_cleanup(&p->mailbox);
	weenet_process_free(p);
}

bool
weenet_process_retire(struct weenet_process *p) {
	p->retired = true;
	weenet_atomic_sync();
	return weenet_process_release(p);
}

struct weenet_process *
weenet_process_retain(struct weenet_process *p) {
	weenet_atomic_add(&p->refcnt, 1);
	return p;
}

// Called by unregister
bool
weenet_process_release(struct weenet_process *p) {
	int64_t ref = weenet_atomic_sub(&p->refcnt, 1);
	assert(ref >= 0);
	if (ref == 0) {
		if (!p->retired) {
			weenet_logger_fatalf("%s(%ld) unexpected terminated.\n", p->name, (long)p->id);
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
	struct weenet_message *m = weenet_mailbox_pop(&p->mailbox);
	if (m == NULL) return false;
	weenet_service_handle(p->service, p, m);
	weenet_message_unref(m);
	return p->wait.session == SESSION_ZERO;
}

void
weenet_process_wait(struct weenet_process *p, session_t sid) {
	p->wait.session = sid;
}

void
weenet_process_mail(struct weenet_process *p, struct weenet_message *m) {
	if (p->retired) {
		weenet_message_unref(m);
		return;
	}
	if (p->wait.session != 0 && p->wait.session == m->session) {
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

void
weenet_process_monitor(struct weenet_process *p, struct weenet_process *dst) {
	// FIXME
	(void)p; (void)dst;
//	weenet_monitor_record(&p->monitor, m, MONITING);
//	weenet_monitor_record(&m->monitor, p, MONITORED);
}

void
weenet_process_demonitor(struct weenet_process *p, struct weenet_process *dst) {
	// FIXME
	(void)p; (void)dst;
}
