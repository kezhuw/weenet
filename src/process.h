#ifndef __WEENET_PROCESS_H_
#define __WEENET_PROCESS_H_

#include "types.h"

#include <stdbool.h>

struct weenet_process;

struct weenet_process * weenet_process_new(const char *name, uintptr_t data, uintptr_t meta);
session_t weenet_process_sid(struct weenet_process *p);
process_t weenet_process_pid(const struct weenet_process *p);
const char *weenet_process_name(const struct weenet_process *p);

process_t weenet_process_self();
struct weenet_process *weenet_process_running();

void weenet_process_push(struct weenet_process *p, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta);
bool weenet_process_resume(struct weenet_process *p);

void weenet_process_wait(struct weenet_process *p, session_t sid);

session_t weenet_process_cast(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta);
session_t weenet_process_call(struct weenet_process *p, process_t dst, uint32_t tags, uintptr_t data, uintptr_t meta);
bool weenet_process_send(process_t dst, process_t src, session_t sid, uint32_t tags, uintptr_t data, uintptr_t meta);

session_t weenet_process_request(process_t pid, uint32_t ridx, uint32_t code, uintptr_t data, uintptr_t meta);
bool weenet_process_notify(process_t pid, uint32_t ridx, uint32_t code, uintptr_t data, uintptr_t meta);
bool weenet_process_response(process_t pid, session_t session, uint32_t ridx, uint32_t code, uintptr_t data, uintptr_t meta);

struct weenet_process *weenet_process_retain(struct weenet_process *p);
bool weenet_process_release(struct weenet_process *p);
void weenet_process_retire(struct weenet_process *p);
void weenet_process_suicide();


int weenet_init_process();

enum wmessage_kind {
	WMSG_KIND_NOTIFY		= 0x00,
	WMSG_KIND_REQUEST		= 0x01,
	WMSG_KIND_RESPONSE		= 0x02,
	WMSG_KIND_BROADCAST		= 0x03,

	WMSG_KIND_MASK			= 0x03,
};

enum wmessage_code {
	WMSG_CODE_TEXT		= 0,
	WMSG_CODE_ERROR		= 2,
	WMSG_CODE_CLIENT	= 3,
	WMSG_CODE_SYSTEM	= 4,
	WMSG_CODE_FILE		= 6,
	WMSG_CODE_EVENT		= 7,
	WMSG_CODE_TIMEO		= 8,
	WMSG_CODE_RETIRE	= 11,
	WMSG_CODE_MONITOR	= 12,
	WMSG_CODE_RETIRED	= 13,
	WMSG_CODE_UDEF_START	= 32767,
	WMSG_CODE_UDEF_STOP	= 65535,

	WMSG_CODE_MASK		= 0x0000FFFF,
};

// resource_id	collect message content after no-one-knows.
enum wmessage_resource_id {
	// resource finalization function has the form:
	// 	void finalize(void *ud, uintptr_t data, uintptr_t meta);
	WMSG_RIDX_MASK		= 255,
	WMSG_RIDX_NONE		= 0,	// no finalization needed

	WMSG_RIDX_UDEF_START	= 1,
	WMSG_RIDX_UDEF_STOP	= 207,

	// weenet will register these with corresponding finalization function
	WMSG_RIDX_RESERVED	= 208,	// [RESERVED, 255] predefined usage
	WMSG_RIDX_LOG		= 251,
	WMSG_RIDX_PROC		= 252,	// 'data' is retained process
	WMSG_RIDX_FILE		= 253,
	WMSG_RIDX_RAWMEM	= 254,
	WMSG_RIDX_MEMORY	= 255,
};

union message_tags {
	uint32_t value;
	struct {
		uint32_t ridx : 8;
		uint32_t kind : 2;
		uint32_t flag : 6;
		uint32_t code : 16;
	};
};

struct weenet_message {
	process_t source;
	session_t session;
	uintptr_t data;
	uintptr_t meta;
	union message_tags tags;
	int32_t refcnt;
};

inline static uint32_t weenet_combine_tags(uint32_t ridx, uint32_t kind, uint32_t code) {
	union message_tags tags = {.ridx = ridx, .kind = kind, .code = code};
	return tags.value;
}

inline static uint32_t weenet_message_code(struct weenet_message *msg);

typedef void (*resource_fini_t)(void *ud, uintptr_t data, uintptr_t meta);

int weenet_message_gc(uint8_t id, void *ud, resource_fini_t fn);

// Take the ownership of the resource.
void weenet_message_take(struct weenet_message *msg);

struct weenet_message *weenet_message_ref(struct weenet_message *msg);
void weenet_message_copy(struct weenet_message *msg, unsigned n);
void weenet_message_unref(struct weenet_message *msg);

void weenet_process_mail(struct weenet_process *p, struct weenet_message *m);

inline static uint32_t
weenet_message_code(struct weenet_message *msg) {
	return msg->tags.code;
}

inline static uint32_t
weenet_message_kind(struct weenet_message *msg) {
	return msg->tags.kind;
}

// If a response timeout message is desired, use WMSG_KIND_REQUEST as flag,
// otherwise flag should be 0.
session_t weenet_process_timeout(uint64_t msecs, uintreg_t flag);

monitor_t weenet_process_monitor(struct weenet_process *p);
void weenet_process_demonitor(monitor_t mref);
#endif
