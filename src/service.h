#ifndef __WEENET_SERVICE_H_
#define __WEENET_SERVICE_H_

#include "atom.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

int weenet_init_service(const char *path);

// Return values:
//
// ESRCH	No library with 'name' find;
// EEXIST	The version of new library is same as older;
// ENOENT	There is no old library loaded, and 'name' library just loaded.
int weenet_library_reload(struct weenet_atom *name);

// Return values:
//
// ENOENT means that no library with 'name' is loaded;
// EBUSY means that there are processes runing code loaded by 'name' library,
// 	and library will be unload after all these processes were retired.
int weenet_library_unload(struct weenet_atom *name);

struct weenet_interface;

// false if already exist.
bool weenet_library_inject(struct weenet_atom *name, const struct weenet_interface *interface);

struct weenet_process;
struct weenet_message;

struct weenet_service;

enum {
	WPATH_GET		= 1,
	WPATH_NEW		= 2,
	WPATH_APPEND		= 3,
	WPATH_PREPEND		= 4,
	WPATH_RETURN		= 5,
};

// Every successfullly call of weenet_library_path() (except RETURN),
// must follow by RETURN to release it to other thread.
const char *weenet_library_path(const char *path, size_t len, int op);


struct weenet_service *weenet_service_new(struct weenet_atom *name, struct weenet_process *p, uintptr_t data, uintptr_t meta);
void weenet_service_delete(struct weenet_service *c, struct weenet_process *p);
int weenet_service_handle(struct weenet_service *c, struct weenet_process *p, struct weenet_message *m);


// Interfaces for library authors. {{
struct weenet_instance;		// enfore strong typed system
typedef struct weenet_instance *(*service_new_t)(struct weenet_process *p, uintptr_t data, uintptr_t meta);
typedef void (*service_delete_t)(struct weenet_instance *c, struct weenet_process *p);
typedef int (*service_handle_t)(struct weenet_instance *c, struct weenet_process *p, struct weenet_message *msg);
typedef void *(*service_reload_t)(struct weenet_instance *c, struct weenet_process *p, uint32_t oldVersion);

typedef int (*library_init_t)();
typedef void (*library_fini_t)();

struct weenet_interface {
	uint32_t global;
	uint32_t version;
	library_init_t init;
	library_fini_t fini;
	service_new_t new;
	service_delete_t delete;
	service_handle_t handle;
	service_reload_t reload;
};
// }}

#endif
