#include "event.h"
#include "config.h"
#include "logger.h"
#include "memory.h"
#include "process.h"
#include "service.h"
#include "schedule.h"
#include "timer.h"

#include <lua5.2/lua.h>
#include <lua5.2/lualib.h>
#include <lua5.2/lauxlib.h>

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <unistd.h>

#include <sys/resource.h>

struct service {
	const char *name;
	uintptr_t data;
	uintptr_t meta;
};

static struct service *
_get_services(lua_State *L, size_t *nump) {
	size_t num = *nump =  0;
	struct service *services = NULL;
	lua_getglobal(L, "services");
	if (lua_istable(L, -1)) {
		int n = (int)lua_rawlen(L, -1);
		for (int i=1; i<=n; ++i) {
			// get the i-th start service
			lua_rawgeti(L, -1, i);

			if (!lua_istable(L, -1)) {
				goto error;
			}

			// get service's name
			lua_rawgeti(L, -1, 1);
			if (lua_type(L, -1) != LUA_TSTRING) {
				goto error;
			}
			const char *name = lua_tostring(L, -1);
			lua_pop(L, 1);

			// get services's data
			uintptr_t data;
			lua_rawgeti(L, -1, 2);
			switch (lua_type(L, -1)) {
			case LUA_TSTRING:
				data = (uintptr_t)lua_tostring(L, -1);
				break;
			case LUA_TNUMBER:
				data = (uintptr_t)lua_tointeger(L, -1);
				break;
			default:
				goto error;
			}
			lua_pop(L, 1);

			// get service's meta
			uintptr_t meta;
			lua_rawgeti(L, -1, 3);
			switch (lua_type(L, -1)) {
			case LUA_TSTRING:
				meta = (uintptr_t)lua_tostring(L, -1);
				break;
			case LUA_TNUMBER:
				meta = (uintptr_t)lua_tointeger(L, -1);
				break;
			default:
				goto error;
			}
			lua_pop(L, 1);

			lua_pop(L, 1);	// pop i-th service table

			if (((num+1) ^ num) > num) {
				services = wrealloc(services, (2*num + 1)*sizeof(struct service));
			}
			services[num].name = name;
			services[num].data = data;
			services[num].meta = meta;
			++num;
			continue;
error:
			fprintf(stderr, "invalid start service in services table[%d].\n", i);
			exit(-1);
		}
	}
	lua_pop(L, 1);
	*nump = num;
	return services;
}

static void
_set_sig_handlers() {
	signal(SIGPIPE, SIG_IGN);
}

static void
_set_open_files_limit(int max) {
	rlim_t maxfiles = (rlim_t)(max+1024);
	struct rlimit limit;
	if (getrlimit(RLIMIT_NOFILE, &limit) < 0) {
		perror("getrlimit(RLIMIT_NOFILE)");
		exit(1);
	}
	if (limit.rlim_cur < maxfiles) {
		struct rlimit expected;
		expected.rlim_cur = maxfiles;
		expected.rlim_max = maxfiles;
		if (setrlimit(RLIMIT_NOFILE, &expected) < 0) {
			fprintf(stderr, "setrlimit(RLIMIT_NOFILE, %ld) failed(%s), current rlimit{.cur = %ld, .max = %ld}\n", (long)maxfiles, strerror(errno), (long)limit.rlim_cur, (long)limit.rlim_max);
			exit(1);
		}
	}
}

int
main(int argc, const char *argv[]) {
	_set_sig_handlers();

	const char *config_file = WEENET_DEFAULT_CONFIG_FILE;
	if (argc >= 3) {
		if (strcmp(argv[1], "-c") != 0) {
			printf("weenet -c config_file\n");
			return -1;
		}
		config_file = argv[2];
	}

	lua_State *L = luaL_newstate();
	luaL_openlibs(L);
	int err = luaL_dofile(L, config_file);
	if (err != 0) {
		fprintf(stderr, "parse config[%s] failed[%s].\n", config_file, lua_tostring(L, -1));
		return -1;
	}

	int max_open_files = WEENET_MAX_OPEN_FILES;
	lua_getglobal(L, "max_open_files");
	if (lua_type(L, -1) != LUA_TNIL) {
		int isnum;
		max_open_files = (int)lua_tointegerx(L, -1, &isnum);
		if (!isnum) {
			fprintf(stderr, "max_open_files expect a number, got %s.\n", luaL_typename(L, -1));
			return -1;
		}
		if (max_open_files < 1024) {
			fprintf(stderr, "max_open_files[%d] smaller than 1024.\n", max_open_files);
		}
	}
	lua_pop(L, 1);
	_set_open_files_limit(max_open_files);

	size_t threads = WEENET_DEFAULT_THREADS;
	lua_getglobal(L, "threads");
	int isnum;
	if (lua_type(L, -1) != LUA_TNIL) {
		threads = (size_t)lua_tointegerx(L, -1, &isnum);
		if (!isnum) {
			fprintf(stderr, "threads expect a number, got %s.\n", luaL_typename(L, -1));
			return -1;
		}
	}
	lua_pop(L, 1);

	const char *log_dir = WEENET_DEFAULT_LOGGER_DIR;
	lua_getglobal(L, "logger_dir");
	if (lua_type(L, -1) == LUA_TSTRING) {
		log_dir = lua_tostring(L, -1);
		assert(log_dir != NULL);
	}
	lua_pop(L, 1);

	const char *service_path = WEENET_DEFAULT_SERVICE_PATH;
	lua_getglobal(L, "service_path");
	if (lua_type(L, -1) == LUA_TSTRING) {
		service_path = lua_tostring(L, -1);
		assert(service_path != NULL);
	}
	lua_pop(L, 1);

	size_t num = 0;
	struct service *services = _get_services(L, &num);

	if (weenet_init_scheduler(threads) != 0) {
		fprintf(stderr, "fail to start scheduler.\n");
		exit(-1);
	}
	if (weenet_init_process() != 0) {
		fprintf(stderr, "fail to start process module.\n");
		exit(-1);
	}
	if (weenet_init_service(service_path) != 0) {
		fprintf(stderr, "fail to start service module.\n");
		exit(-1);
	}
	if (weenet_event_start(102400) != 0) {
		fprintf(stderr, "fail to start event service.\n");
		exit(-1);
	}
	if (weenet_init_time() != 0) {
		fprintf(stderr, "fail to init time module.\n");
		exit(-1);
	}
	if (weenet_init_logger(log_dir, 1024*1024*10) != 0) {
		fprintf(stderr, "fail to start logger service.\n");
		exit(-1);
	}

	if (services != NULL) {
		for (size_t i=0; i<num; ++i) {
			struct service *s = services+i;
			struct weenet_process *p = weenet_process_new(s->name, s->data, s->meta);
			if (p == NULL) {
				fprintf(stderr, "services[%zu name(%s)] failed to start!\n", i, s->name);
				exit(-1);
			}
			weenet_process_release(p);
		}
		wfree(services);
	}

	for (;;) {
		weenet_update_time();
		usleep(1000);
	}

	return 0;
}
