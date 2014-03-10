#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>

#include <weenet.h>

struct timeout {
	uint64_t i;
	uint64_t n;
	uint64_t msecs;
};

static void *
timeout_new(struct weenet_process *p, uintptr_t data, uintptr_t meta) {
	(void)p;
	struct timeout *t = malloc(sizeof(*t));
	t->i = 0;
	t->n = (uint64_t)data;
	t->msecs = (uint64_t)meta;
	printf("new timeout service[%p]:\n", t);
	printf("n[%" PRIu64 "] msecs[%" PRIu64 "]\n", t->n, t->msecs);
	weenet_process_timeout(t->msecs);
	return t;
}

static int
timeout_handle(struct timeout *t, struct weenet_process *p, struct weenet_message *m) {
	(void)m;
	uint64_t i = ++t->i;
	printf("timeout %" PRIu64 "\n", i);

	if (i < t->n) {
		weenet_process_timeout(t->msecs);
	} else {
		weenet_process_retire(p);
	}
	return 0;
}

static void
timeout_delete(struct timeout *t) {
	assert(t->i == t->n);
	free(t);
	printf("delete timeout service[%p]\n", t);
}

const struct weenet_interface timeout_service = {
	.new		= (service_new_t)timeout_new,
	.handle		= (service_handle_t)timeout_handle,
	.delete		= (service_delete_t)timeout_delete,
};
