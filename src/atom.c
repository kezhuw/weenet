#include "atom.h"
#include "memory.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

struct atom {
	struct atom *link;
	uint32_t hash;
	uint32_t len;
	char str[];
};

enum { SET_SIZE = 4096 };

struct set {
	//size_t num;
	//size_t size;
	struct atom *nodes[SET_SIZE];
};

static struct set S;

enum { BLOCK_SIZE = 8192 };

struct block {
	struct block *next;
	char bytes[];
};

static struct fixed_block {
	struct block *next;
	char bytes[BLOCK_SIZE];
} FIRST;

struct storage {
	char *seq;
	char *end;
	struct block *last;
	struct block *first;
};

static struct storage G = {
	FIRST.bytes, FIRST.bytes + BLOCK_SIZE,
	(struct block *)&FIRST, (struct block *)&FIRST
};

static struct atom *
_search(struct set *s, uint32_t h, const char *str, size_t len) {
	size_t i =  h % SET_SIZE;
	struct atom *a = s->nodes[i];
	while (a != NULL) {
		if (a->str == str || (h == a->hash && len == (size_t)a->len && memcmp(a->str, str, len) == 0)) {
			return a;
		}
		a = a->link;
	}
	return NULL;
}

static void
_insert(struct set *s, struct atom *a) {
	size_t i = a->hash % SET_SIZE;
	a->link = s->nodes[i];
	s->nodes[i] = a;
}

#define ALIGNMENT	(sizeof(void*) - 1)
#define aligned(size)	(((size) + ALIGNMENT) & ~ALIGNMENT)

static struct atom *
_alloc(struct storage *g, size_t len) {
	size_t size = sizeof(struct atom) + len;
	size = aligned(size);
	if (size >= BLOCK_SIZE) {
		// XXX assertion failure ?!
		struct block *b = wmalloc(sizeof(*b) + size);
		b->next = g->first->next;
		g->first->next = b;
		return (struct atom *)b->bytes;
	} else if (g->seq + size > g->end) {
		struct block *b = wmalloc(sizeof(*b) + BLOCK_SIZE);
		b->next = g->last->next;
		g->last->next = b;
		g->last = b;
		g->seq = b->bytes;
		g->end = b->bytes + BLOCK_SIZE;
	}
	assert(g->seq + size <= g->end);
	struct atom *a = (struct atom *)g->seq;
	g->seq += size;
	return a;
}

static struct atom *
_new(struct storage *g, uint32_t h, const char *str, size_t len) {
	struct atom *a = _alloc(g, len+1);
	a->len = (uint32_t)len;
	a->hash = h;
	memcpy(a->str, str, len);
	a->str[len] = '\0';
	return a;
}

#define SEED	((uint32_t)(((uintptr_t)&_hash + (uintptr_t)&G) >> 3))

inline static uint32_t
_hash(const char *str, size_t len) {
	uint32_t hash = SEED;
	size_t n = (len < 0x10) ? len : 0x10;
	for (size_t i=0; i<n; ++i) {
		hash += (uint32_t)(unsigned char)str[i];
	}
	return hash;
}

#include "atomic.h"

static int64_t L;
#define LOCK()		weenet_atomic_lock(&L)
#define UNLOCK()	weenet_atomic_unlock(&L)

#define RETURN(a)	((struct weenet_atom *)((char *)a + offsetof(struct atom, len)))

struct weenet_atom *
weenet_atom_new(const char *str, size_t len) {
	assert(str != NULL);
	uint32_t h = _hash(str, len);
	LOCK();
	struct atom *a = _search(&S, h, str, len);
	if (a == NULL) {
		a = _new(&G, h, str, len);
		_insert(&S, a);
	}
	UNLOCK();
	return RETURN(a);
}

bool
weenet_atom_equal(const struct weenet_atom *a, const struct weenet_atom *b) {
	return a == b;
}

int
weenet_atom_compare(const struct weenet_atom * restrict a, const struct weenet_atom * restrict b) {
	if (weenet_atom_equal(a, b)) {
		return 0;
	}
	int min = (int)(a->len < b->len ? a->len : b->len);
	int ret = memcmp(a->str, b->str, (size_t)min);
	if (ret == 0) {
		ret = (int)(a->len - b->len);
	}
	return ret;
}
