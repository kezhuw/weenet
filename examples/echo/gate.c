#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <weenet.h>
#include <unistd.h>

#include <sys/socket.h>

struct gate {
	process_t self;
	struct weenet_process *listener;
	char address[];
};

static struct gate *
gate_new(struct weenet_process *p, const char *address) {
	struct weenet_process *listener = weenet_process_new("listener", (uintptr_t)address, SOMAXCONN);
	if (listener == NULL) return NULL;

	size_t len = strlen(address);
	struct gate *g = wmalloc(sizeof(*g) + len + 1);
	process_t self = weenet_process_pid(p);
	g->self = self;
	g->listener = listener;
	memcpy(g->address, address, len+1);

	uint32_t tags = weenet_combine_tags(WMSG_RIDX_PROC, 0, WMSG_CODE_TEXT);
	weenet_process_push(listener, self, 0, tags, (uintptr_t)weenet_process_retain(p), 0);
	weenet_process_monitor(listener);
	weenet_process_release(listener);

	return g;
}

static void
gate_delete(struct gate *g) {
	wfree(g);
};

static int
gate_handle(struct gate *g, struct weenet_process *p, struct weenet_message *m) {
	(void)p;
	uint32_t code = weenet_message_code(m);
	switch (code) {
	case WMSG_CODE_RETIRED:
		g->listener = NULL;
		weenet_logger_errorf("listern(%s) unexpected retired!\n", g->address);
		break;
	case WMSG_CODE_FILE:
		weenet_message_take(m);
		int fd = (int)m->data;
		struct weenet_process *agent = weenet_process_new("agent", (uintptr_t)fd, 0);
		if (agent == NULL) {
			weenet_logger_fatalf("agent start failed.\n");
			close(fd);
			return -1;
		}
		weenet_process_release(agent);
		//int fd = (int)m->data;
		//char buf[48];
		//sprintf(buf, "%d", fd);
		//struct weenet_process *agent = weenet_process_new("lua", "agent", buf);
		//if (agent == NULL) {
		//	weenet_logger_fatalf("weenet_process_new(%s, %s) failed.\n", name, args);
		//	close(fd);
		//	return -1;
		//}
		//// XXX Prefered to send file message ?
		//// uint32_t tags = weenet_combine_tags(WMSG_RIDX_FILE, 0, WMSG_CODE_FILE);
		//// weenet_process_push(agent, g->self, 0, tags, (uintptr_t)fd, 0);
		//weenet_process_release(agent);
		break;
	default:
		break;
	}

	return 0;
}

const struct weenet_interface gate_service = {
	.new		= (service_new_t)gate_new,
	.delete		= (service_delete_t)gate_delete,
	.handle		= (service_handle_t)gate_handle,
};
