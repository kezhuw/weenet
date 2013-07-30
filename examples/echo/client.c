#include <string.h>
#include <weenet.h>

struct client {
	struct weenet_process *agent;
};

static struct client *
client_new(struct weenet_process *p, uintptr_t data, uintptr_t meta) {
	(void)meta;
	struct weenet_process *agent = (struct weenet_process *)data;
	if (agent == NULL) return NULL;

	struct client *e = wmalloc(sizeof(*e));
	e->agent = agent;
	weenet_process_monitor(p, agent);
	return e;
}

static void
client_delete(struct client *e) {
	if (e->agent != NULL) weenet_process_retire(e->agent);
	wfree(e);
}

static int
client_handle(struct client *e, struct weenet_process *p, struct weenet_message *m) {
	uint32_t type = weenet_message_type(m);
	switch (type) {
	case WMESSAGE_TYPE_RETIRED:
		e->agent = NULL;
		break;
	case WMESSAGE_TYPE_CLIENT:
		;size_t size = (size_t)m->meta;
		if (size == 0) {
			weenet_process_retire(p);
		} else if (e->agent != NULL) {
			void *dup = wmalloc(size);
			memcpy(dup, (void*)m->data, size);
			weenet_process_push(e->agent, weenet_process_self(p), 0, WMESSAGE_TYPE_CLIENT|WMESSAGE_RIDX_MEMORY, (uintptr_t)dup, (uintptr_t)size);
		}
		break;
	default:
		break;
	}
	return 0;
}

const struct weenet_interface client_service = {
	.new		= (service_new_t)client_new,
	.delete		= (service_delete_t)client_delete,
	.handle		= (service_handle_t)client_handle,
};
