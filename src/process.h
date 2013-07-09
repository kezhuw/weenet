#ifndef __WEENET_PROCESS_H_
#define __WEENET_PROCESS_H_

#include "types.h"

#include <stdbool.h>

struct weenet_process;

struct weenet_process * weenet_process_new(const char *name, uintptr_t data, uintptr_t meta);
session_t weenet_process_sid(struct weenet_process *p);
process_t weenet_process_self(const struct weenet_process *p);

void weenet_process_push(struct weenet_process *p, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta);
bool weenet_process_resume(struct weenet_process *p);

void weenet_process_wait(struct weenet_process *p, session_t sid);

session_t weenet_process_boot(struct weenet_process *p, uint32_t tags, uintptr_t data, uintptr_t meta);
session_t weenet_process_cast(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta);
session_t weenet_process_call(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta);
int weenet_process_send(process_t dst, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta);

struct weenet_process *weenet_process_retain(struct weenet_process *p);
bool weenet_process_release(struct weenet_process *p);
void weenet_process_retire(struct weenet_process *p);


int weenet_init_process();

// FIXME (type, flag, resource_id)
//
// resource_id	collect message content after no-one-knows.
//
// type ???
//
// flag ???
// 	FLAG_REQUEST		// message that expect a response
// 	FLAG_RESPONSE		// message that response other's request
enum wmessage_info {
	// resource finalization function has the form:
	// 	void finalize(void *ud, uintptr_t data, uintptr_t meta);
	//
	// resource id start {{
	WMESSAGE_RIDX_MASK		= 255,
	WMESSAGE_RIDX_NONE		= 0,	// no finalization needed

	WMESSAGE_RIDX_UDEF_START	= 1,
	WMESSAGE_RIDX_UDEF_STOP		= 207,

	// weenet will register these with corresponding finalization function
	WMESSAGE_RIDX_RESERVED		= 208,	// [RESERVED, 255] predefined usage
	WMESSAGE_RIDX_LOG		= 251,
	WMESSAGE_RIDX_PROC		= 252,	// 'data' is retained process
	WMESSAGE_RIDX_FILE		= 253,
	WMESSAGE_RIDX_RAWMEM		= 254,
	WMESSAGE_RIDX_MEMORY		= 255,
	// resource id end }}

	WMESSAGE_TYPE_MASK		= 0xFFFF0000,
	WMESSAGE_TYPE_TEXT		= 0,
	WMESSAGE_TYPE_ERROR		= 2 << 16,
	WMESSAGE_TYPE_CLIENT		= 3 << 16,
	WMESSAGE_TYPE_SYSTEM		= 4 << 16,
	WMESSAGE_TYPE_BOOT		= 5 << 16,
	WMESSAGE_TYPE_FILE		= 6 << 16,
	WMESSAGE_TYPE_EVENT		= 7 << 16,
	WMESSAGE_TYPE_TIMEO		= 8 << 16,
	WMESSAGE_TYPE_DUMMY		= 9 << 16,
	WMESSAGE_TYPE_WAKEUP		= 10 << 16,	// delete it ? equal to DUMMY
	WMESSAGE_TYPE_RETIRE		= 11 << 16,
	WMESSAGE_TYPE_DEMONITOR		= 12 << 16,
	WMESSAGE_TYPE_RETIRED		= 13 << 16,
	WMESSAGE_TYPE_UDEF_START	= 32767 << 16,
	WMESSAGE_TYPE_UDEF_STOP		= 65535 << 16,

	WMESSAGE_FLAG_REQUEST		= 0x0100,
	WMESSAGE_FLAG_RESPONSE		= 0x0200,
	WMESSAGE_FLAG_INTERNAL		= 0x0400,

	WMESSAGE_TAGS_RETIRED		= WMESSAGE_TYPE_RETIRED | WMESSAGE_RIDX_PROC | WMESSAGE_FLAG_INTERNAL,
};

struct weenet_message {
	process_t source;
	session_t session;
	uintptr_t data;
	uintptr_t meta;
	uint32_t tags;
	int32_t refcnt;
};

inline static uint32_t weenet_message_type(struct weenet_message *msg);
inline static uint32_t weenet_message_ridx(struct weenet_message *msg);

int weenet_message_gc(uint32_t id, void *ud, void (*fini)(void *ud, uintptr_t data, uintptr_t meta));
void weenet_message_ref(struct weenet_message *msg);
void weenet_message_copy(struct weenet_message *msg, int n);
void weenet_message_unref(struct weenet_message *msg);

void weenet_process_mail(struct weenet_process *p, struct weenet_message *m);

inline static uint32_t
weenet_message_type(struct weenet_message *msg) {
	return (msg->tags & WMESSAGE_TYPE_MASK);
}

inline static uint32_t
weenet_message_ridx(struct weenet_message *msg) {
	return (msg->tags & WMESSAGE_RIDX_MASK);
}

session_t weenet_process_timeo(struct weenet_process *p, uint64_t msecs);
uintreg_t weenet_process_monitor(struct weenet_process *p, struct weenet_process *dst);
void weenet_process_demonitor(struct weenet_process *p, uintreg_t mref);
#endif
