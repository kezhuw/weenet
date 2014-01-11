#include "atom.h"
#include "atomic.h"
#include "config.h"
#include "logger.h"
#include "memory.h"
#include "process.h"
#include "slab.h"
#include "service.h"
#include "types.h"
#include "utils.h"

#include <errno.h>
#include <dlfcn.h>

#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct shared_dynamic;

struct weenet_library {
	const char *name;
	const struct weenet_interface *interface;
	struct shared_dynamic *dynamic;
	struct weenet_library *upgrade;
	int64_t refcnt;		// global reference held by M
};

struct weenet_service {
	void *instance;
	struct weenet_library *library;
};

struct node {
	const char *name;
	struct weenet_library *value;
	struct node *link;
};

#define MAP_SIZE	4096
#define NODE_NUM	4096

struct block {
	struct block *link;
	struct node *seq;
	struct node nodes[NODE_NUM];
};

struct map {
	int64_t lock;
	struct node *nodes[MAP_SIZE];
	struct node *frees;
	struct block first;
	struct block *last;
};

// Hold the global reference of 'struct weenet_library'.
#define _lock_map(m)		weenet_atomic_lock(&m->lock)
#define _unlock_map(m)		weenet_atomic_unlock(&m->lock)

static struct map M = {
	.nodes = {NULL},
	.first = {.link = NULL, .seq = M.first.nodes},
	.last = &M.first,
};

#define PATH_LEN	4096

struct path {
	int64_t lock;
	size_t len;
	char buf[PATH_LEN+1];
};

static struct path P = {
	.lock = 0,
	.len = sizeof(WEENET_DEFAULT_SERVICE_PATH)-1,
	.buf = WEENET_DEFAULT_SERVICE_PATH,
};

static struct node *
_new_node(struct map *m) {
	struct node *n = m->frees;
	if (n != NULL) {
		m->frees = n->link;
		return n;
	}
	if (m->last->seq == &m->last->nodes[NODE_NUM]) {
		struct block *b = wmalloc(sizeof(*b));
		b->seq = b->nodes;
		b->link = m->last;
		m->last = b;
	}
	return m->last->seq++;
}

static void
_free_node(struct map *m, struct node *n) {
	n->link = m->frees;
	m->frees = n;
}

#define _hash(a)	((uint32_t)((uintptr_t)a >> 3))

static struct weenet_library *
_search(struct map *m, const char *name) {
	uint32_t i = _hash(name) % nelem(m->nodes);
	struct node *n = m->nodes[i];
	while (n != NULL) {
		if (n->name == name) {
			return n->value;
		}
		n = n->link;
	}
	return NULL;
}

static struct weenet_library *
_delete(struct map *m, const char *name) {
	uint32_t i = _hash(name) % nelem(m->nodes);
	struct node *n = m->nodes[i];
	struct node **np = &m->nodes[i];
	while (n != NULL) {
		if (n->name == name) {
			*np = n->link;
			struct weenet_library *lib = n->value;
			_free_node(m, n);
			return lib;
		}
		np = &n->link;
		n = n->link;
	}
	return NULL;
}

static void
_insert(struct map *m, struct weenet_library *lib) {
	const char *name = lib->name;
	uint32_t i = _hash(name) % nelem(m->nodes);
	struct node *n = _new_node(m);
	n->name = name;
	n->value = lib;
	n->link = m->nodes[i];
	m->nodes[i] = n;
}

static struct weenet_library *
_replace(struct map *m, struct weenet_library *lib) {
	const char *name = lib->name;
	uint32_t i = _hash(name) % nelem(m->nodes);
	struct node *n = m->nodes[i];
	while (n != NULL) {
		if (n->name == name) {
			struct weenet_library *old = n->value;
			n->value = lib;
			return old;
		}
		n = n->link;
	}
	n = _new_node(m);
	n->name = name;
	n->value = lib;
	n->link = m->nodes[i];
	m->nodes[i] = n;
	return NULL;
}

static struct slab *library_slab;

static struct weenet_library *
weenet_library_new(const char *name, struct shared_dynamic *dynamic, const struct weenet_interface *interface) {
	struct weenet_library *lib = slab_retain(library_slab);
	lib->name = name;
	lib->interface = interface;
	lib->upgrade = NULL;
	lib->dynamic = dynamic;
	lib->refcnt = 1;
	return lib;
}

static bool weenet_library_unref(struct weenet_library *lib);

static void
weenet_library_delete(struct weenet_library *lib) {
	if (lib->upgrade) {
		weenet_library_unref(lib->upgrade);
	}
	if (lib->interface->fini != NULL) {
		lib->interface->fini();
	}
	if (lib->dynamic) {
		dlclose(lib->dynamic);
	}
	slab_release(library_slab, lib);
}

static struct weenet_library *
weenet_library_ref(struct weenet_library *lib) {
	weenet_atomic_inc(&lib->refcnt);
	return lib;
}

static bool
weenet_library_unref(struct weenet_library *lib) {
	int64_t refcnt = weenet_atomic_dec(&lib->refcnt);
	if (refcnt == 0) {
		weenet_library_delete(lib);
		return true;
	}
	return false;
}

#define _lock_path(p)		weenet_atomic_lock(&p->lock)
#define _unlock_path(p)		weenet_atomic_unlock(&p->lock)

#define SUFFIX		"_service"

static struct weenet_library *
_open(struct path *p, const char *name, size_t nlen) {
	_lock_path(p);
	char buf[p->len+1];
	memcpy(buf, p->buf, p->len+1);
	_unlock_path(p);
	char *it = buf;
	char *patt;
	char exported[nlen + sizeof(SUFFIX)];
	memcpy(exported, name, nlen);
	memcpy(exported+nlen, SUFFIX, sizeof(SUFFIX));
	while ((patt = strsep(&it, ":"))) {
		size_t plen = strlen(patt);
		size_t size = plen + nlen;
		char filename[size];
		const char *mark = memchr(patt, '?', plen);
		if (mark == NULL) continue;
		size_t dist = (size_t)(mark-patt);
		memcpy(filename, patt, dist);
		memcpy(filename+dist, name, nlen);
		memcpy(filename+dist+nlen, mark+1, plen-dist-1);
		filename[size-1] = '\0';

		struct shared_dynamic *dynamic = dlopen(filename, RTLD_NOW);
		if (dynamic == NULL) {
			//char *err = dlerror();
			//if (err != NULL) {
			//	fprintf(stdout, "dlopen(%s, RTLD_NOW) failed[%s]\n", filename, err);
			//}
			continue;
		}

		const struct weenet_interface *interface = dlsym(dynamic, exported);
		if (interface == NULL || interface->new == NULL || interface->handle == NULL) {
			if (interface != NULL) {
				weenet_logger_errorf("dynamic[%s file(%s)] has no new or handle function.\n", name, filename);
			}
			dlclose(dynamic);
			continue;
		}
		if (interface->global) {
			void *tmp = dlopen(filename, RTLD_NOLOAD|RTLD_GLOBAL);
			assert(tmp == dynamic);
			assert(interface == dlsym(dynamic, exported));
			dlclose(tmp);
		}
		if (interface->init != NULL) {
			if (interface->init() != 0) {
				weenet_logger_errorf("dynamic[%s file(%s)] init failed.\n", name, filename);
				dlclose(dynamic);
				continue;
			}
		}
		return weenet_library_new(name, dynamic, interface);
	}
	return NULL;
}

static struct weenet_library *
weenet_library_open(struct weenet_atom *name) {
	struct map *m = &M;
	_lock_map(m);
	struct weenet_library *lib = _search(m, weenet_atom_str(name));
	if (lib == NULL) {
		lib = _open(&P, weenet_atom_str(name), weenet_atom_len(name));
		if (lib == NULL) {
			_unlock_map(m);
			return NULL;
		}
		_insert(m, lib);
	}
	weenet_library_ref(lib);
	_unlock_map(m);
	return lib;
}

const char *
weenet_library_path(const char *path, size_t len, int op) {
	struct path *p = &P;
	switch (op) {
	case WPATH_GET:
		_lock_path(p);
		return p->buf;
	case WPATH_NEW:
		_lock_path(p);
		//TODO :: means default path
		if (len <= PATH_LEN) {
			p->len = len;
			memcpy(p->buf, path, len);
			p->buf[len] = '\0';
			return p->buf;
		}
		_unlock_path(p);
		break;
	case WPATH_APPEND:
		_lock_path(p);
		if (len + p->len + 1 <= PATH_LEN) {
			p->buf[p->len] = ':';
			memcpy(p->buf+p->len+1, path, len);
			p->len += len+1;
			p->buf[p->len] = '\0';
			return p->buf;
		}
		_unlock_path(p);
		break;
	case WPATH_PREPEND:
		_lock_path(p);
		if (len + p->len + 1 <= PATH_LEN) {
			memmove(p->buf+len+1, p->buf, p->len+1);
			memcpy(p->buf, path, len);
			p->buf[len] = ':';
			p->len += len+1;
			return p->buf;
		}
		_unlock_path(p);
		break;
	case WPATH_RETURN:
		_unlock_path(p);
		break;
	}
	return NULL;
}

int
weenet_library_reload(struct weenet_atom *name) {
	struct weenet_library *lib = _open(&P, weenet_atom_str(name), weenet_atom_len(name));
	if (lib == NULL) return ESRCH;

	bool dup = false;
	struct map *m = &M;
	_lock_map(m);
	struct weenet_library *old = _replace(m, lib);
	if (old != NULL && old->interface->version == lib->interface->version) {
		_replace(m, old);
		dup = true;
	}
	_unlock_map(m);

	if (dup) {
		bool freed = weenet_library_unref(lib);
		assert(freed == true);
		(void)freed;
		return EEXIST;
	}
	if (old == NULL) return ENOENT;

	// XXX Passive reloading now.
	// Aggressive reloading need more bookkeeping.
	old->upgrade = weenet_library_ref(lib);
	weenet_library_unref(old);	// unref global reference
	return 0;
}

int
weenet_library_unload(struct weenet_atom *name) {
	struct map *m = &M;
	_lock_map(m);
	struct weenet_library *lib = _delete(m, weenet_atom_str(name));
	_unlock_map(m);
	if (lib == NULL) {
		return ENOENT;
	}
	bool none = weenet_library_unref(lib);
	if (!none) {
		return EBUSY;
	}
	return 0;
}

bool
weenet_library_inject(struct weenet_atom *name, struct weenet_interface *interface) {
	struct map *m = &M;
	_lock_map(m);
	struct weenet_library *lib = _search(m, weenet_atom_str(name));
	if (lib != NULL) {
		_unlock_map(m);
		return false;
	}
	lib = weenet_library_new(weenet_atom_str(name), NULL, interface);
	_insert(m, lib);
	_unlock_map(m);
	return true;
}

#define _new_service()		((struct weenet_service *)wmalloc(sizeof(struct weenet_service)))
#define _free_service(c)	wfree(c);

struct weenet_service *
weenet_service_new(struct weenet_atom *name, struct weenet_process *p, uintptr_t data, uintptr_t meta) {
	struct weenet_library *lib = weenet_library_open(name);
	if (lib == NULL) return NULL;
	void *instance = lib->interface->new(p, data, meta);
	if (instance == NULL) {
		weenet_library_unref(lib);
		return NULL;
	}
	struct weenet_service *c = _new_service();
	c->library = lib;		// already referenced
	c->instance = instance;
	return c;
}

void
weenet_service_delete(struct weenet_service *c, struct weenet_process *p) {
	(void)p;
	struct weenet_library *lib = c->library;
	if (lib->interface->delete) {
		lib->interface->delete(c->instance, p);
	}
	weenet_library_unref(lib);
	_free_service(c);
}

int
weenet_service_handle(struct weenet_service *c, struct weenet_process *p, struct weenet_message *m) {
	struct weenet_library *lib = c->library;
	if (lib->upgrade != NULL) {
		struct weenet_library *upgrade = lib->upgrade;
		if (upgrade->interface->reload != NULL) {
			void *instance = upgrade->interface->reload(c->instance, p, lib->interface->version);
			if (instance == NULL) {
				weenet_logger_errorf("process[%ld name(%s)] reload failed!\n", (long)weenet_process_self(p), lib->name);
				goto handle_message;
			}
			c->instance = instance;
		}
		struct weenet_library *old = lib;
		lib = c->library = weenet_library_ref(upgrade);
		weenet_library_unref(old);
	}
handle_message:
	return lib->interface->handle(c->instance, p, m);
}

int
weenet_init_service(const char *path) {
	library_slab = slab_new(32, sizeof(struct weenet_library));
	if (path != NULL) {
		weenet_library_path(path, strlen(path), WPATH_NEW);
		weenet_library_path(NULL, 0, WPATH_RETURN);
	}
	return 0;
}
