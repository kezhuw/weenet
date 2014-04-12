#include "types.h"
#include "utils.h"
#include "logger.h"
#include "memory.h"
#include "process.h"
#include "service.h"

#include <lua5.2/lua.h>
#include <lua5.2/lualib.h>
#include <lua5.2/lauxlib.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

struct lua {
	struct lua_State *L;
	process_t self;
	const char *name;
	const char *filepath;
};

// lua

static struct weenet_process *
_get_process(lua_State *L) {
	return lua_touserdata(L, lua_upvalueindex(1));
}

static int
_callback(lua_State *L) {
	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, _callback);
	return 0;
}


static int
_new_session(lua_State *L) {
	struct weenet_process *p = _get_process(L);
	session_t session = weenet_process_sid(p);
	lua_pushunsigned(L, (lua_Unsigned)session);
	return 1;
}

static int
_exit(lua_State *L) {
	struct weenet_process *p = _get_process(L);
	weenet_process_retire(p);
	return 0;
}


static int
_self(lua_State *L) {
	struct weenet_process *p = _get_process(L);
	process_t pid = weenet_process_pid(p);
	lua_pushunsigned(L, (lua_Unsigned)pid);
	return 1;
}

static int
_timeout(lua_State *L) {
	uint64_t msecs = (uint64_t)luaL_checkunsigned(L, 1);
	uint32_t session = weenet_process_timeout(msecs, WMSG_KIND_REQUEST);
	lua_pushunsigned(L, (lua_Unsigned)session);
	return 1;
}

static int
_bootstrap(lua_State *L) {
	struct weenet_process *p = _get_process(L);
	session_t session = weenet_process_sid(p);
	uint32_t tags = weenet_combine_tags(0, WMSG_KIND_RESPONSE, 0);
	weenet_process_push(p, weenet_process_pid(p), session, tags, 0, 0);
	lua_pushunsigned(L, (lua_Unsigned)session);
	return 1;
}

static void
_extract_data(lua_State *L, int idx, uint32_t *ridx, uintptr_t *data, uintptr_t *meta) {
	switch (lua_type(L, idx)) {
	case LUA_TUSERDATA:
		*data = (uintptr_t)lua_touserdata(L, idx);
		*meta = (uintptr_t)lua_touserdata(L, idx+1);
		*ridx = (uint32_t)luaL_optunsigned(L, idx+2, 0);
		break;
	case LUA_TSTRING:
		;size_t len;
		const char *str = luaL_checklstring(L, idx, &len);
		size_t size = len+1;
		void *copy = wmalloc(size);
		memcpy(copy, str, size);
		*data = (uintptr_t)copy;
		*meta = (uintptr_t)size;
		*ridx = WMSG_RIDX_MEMORY;
		break;
	default:
		*ridx = 0;
		*data = *meta = 0;
		break;
	}
}

// local session = c.request(address, proto.code, proto.request.pack(...))
static int
_request(lua_State *L) {
	process_t address = (process_t)luaL_checkunsigned(L, 1);
	uint32_t code = (uint32_t)luaL_checkunsigned(L, 2);
	uint32_t ridx = 0;
	uintptr_t data, meta;
	_extract_data(L, 3, &ridx, &data, &meta);
	uint32_t session = weenet_process_request(address, ridx, code, data, meta);
	lua_pushunsigned(L, (lua_Unsigned)session);
	return 1;
}

static int
_response(lua_State *L) {
	process_t pid = (process_t)luaL_checkunsigned(L, 1);
	session_t session = (session_t)luaL_checkunsigned(L, 2);
	uint32_t code = (uint32_t)luaL_checkunsigned(L, 3);
	uint32_t ridx;
	uintptr_t data, meta;
	_extract_data(L, 4, &ridx, &data, &meta);
	bool succ = weenet_process_response(pid, session, ridx, code, data, meta);
	lua_pushboolean(L, succ);
	return 1;
}

//c.send(address, session, PKIND_NOTIFY, proto.code, proto.notify.pack(...))
static int
_send(lua_State *L) {
	struct weenet_process *p = _get_process(L);

	process_t address = (process_t)luaL_checkunsigned(L, 1);

	session_t session;
	if (lua_isnil(L, 2)) {
		session = weenet_process_sid(p);
	} else {
		session = (session_t)luaL_checkunsigned(L, 3);
	}

	uint32_t kind = (uint32_t)luaL_checkunsigned(L, 3);
	uint32_t code = (uint32_t)luaL_checkunsigned(L, 4);

	if (session == 0 && kind == WMSG_KIND_REQUEST) {
		session = weenet_process_sid(p);
	}

	uint32_t ridx = 0;
	uintptr_t data, meta;
	_extract_data(L, 5, &ridx, &data, &meta);

	uint32_t tags = weenet_combine_tags(ridx, kind, code);
	weenet_process_send(address, weenet_process_pid(p), session, tags, data, meta);

	lua_pushunsigned(L, (lua_Unsigned)session);
	return 1;
}

static const luaL_Reg funcs[] = {
	{"new_session", _new_session},
	{"timeout", _timeout},
	{"callback", _callback},
	{"exit", _exit},
	{"self", _self},
	{"send", _send},
	{"request", _request},
	{"response", _response},
	{"bootstrap", _bootstrap},
	{NULL, NULL},
};

static int
luaopen_weenet_c(lua_State *L) {
	luaL_newlibtable(L, funcs);
	lua_rawgetp(L, LUA_REGISTRYINDEX, weenet_process_new);
	luaL_setfuncs(L, funcs, 1);

	return 1;
}

static int
_gc_message(lua_State *L) {
	struct weenet_message *m = lua_touserdata(L, 1);
	weenet_message_unref(m);
	return 0;
}

static int
_index_message(lua_State *L) {
	struct weenet_message *m = lua_touserdata(L, 1);
	const char *key = luaL_checkstring(L, 2);
	if (strcmp(key, "kind") == 0) {
		lua_pushunsigned(L, (lua_Unsigned)m->tags.kind);
		return 1;
	}
	if (strcmp(key, "source") == 0) {
		lua_pushunsigned(L, (lua_Unsigned)m->source);
		return 1;
	}
	if (strcmp(key, "session") == 0) {
		lua_pushunsigned(L, (lua_Unsigned)m->session);
		return 1;
	}
	if (strcmp(key, "content") == 0) {
		lua_pushlightuserdata(L, (void*)m->data);
		lua_pushlightuserdata(L, (void*)m->meta);
		return 2;
	}
	return 0;
}

static void
_create_metatable(lua_State *L) {
	luaL_newmetatable(L, "weenet_message");

	lua_pushcfunction(L, _gc_message);
	lua_setfield(L, -2, "__gc");

	lua_pushcfunction(L, _index_message);
	lua_setfield(L, -2, "__index");

	lua_pop(L, 1);
}

static int
_loadfile(lua_State *L, const char *path, const char *name) {
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "searchpath");
	lua_remove(L, -2);

	lua_pushstring(L, name);
	lua_pushstring(L, path);
	lua_call(L, 2, 2);
	if (lua_isnil(L, -2)) {
		return LUA_ERRFILE;
	}
	lua_pop(L, 1);
	const char *filepath = lua_tostring(L, -1);
	int err = luaL_loadfile(L, filepath);
	return err;
}

static int
_traceback(lua_State *L) {
	const char *msg = lua_tostring(L, -1);
	if (msg) {
		luaL_traceback(L, L, msg, 1);
	} else {
		lua_pushliteral(L, "no error msg");
	}
	return 1;
}

static struct lua *
lua_new(struct weenet_process *p, const char *name) {
	printf("loading service: %s ...\n", name);
	struct lua_State *L = luaL_newstate();
	luaL_openlibs(L);

	lua_pushlightuserdata(L, p);
	lua_rawsetp(L, LUA_REGISTRYINDEX, weenet_process_new);

	luaL_requiref(L, "weenet.c", luaopen_weenet_c, 0);
	lua_pop(L, 1);

	lua_pushcfunction(L, _traceback);
	assert(lua_gettop(L) == 1);

	const char *path = "./?.lua";// FIXME weenet_getenv("lua_service_path");
	int err = _loadfile(L, path, name);
	if (err != 0) {
		weenet_logger_errorf("lua service[%s] fail to load: %s", name, lua_tostring(L, -1));
		lua_close(L);
		return NULL;
	}

	name = lua_pushstring(L, name);
	lua_setglobal(L, "SERVICE_NAME");

	lua_pushvalue(L, -2);
	lua_setglobal(L, "SERVICE_FILENAME");

	const char *filepath = lua_tostring(L, -2);
	err = lua_pcall(L, 0, 0, 1);
	if (err != LUA_OK) {
		weenet_logger_errorf("lua service[%s file(%s)] fail to call: %s", name, filepath, lua_tostring(L, -1));
		lua_close(L);
		return NULL;
	}
	lua_pop(L, 1);	// pop filepath
	assert(lua_gettop(L) == 1);

	_create_metatable(L);

	struct lua *l = wmalloc(sizeof(*l));
	l->L = L;
	l->self = weenet_process_pid(p);
	l->name = name;
	l->filepath = filepath;
	return l;
}

// local function dispatch_message(source, session, kind, code, data, meta)
static int
lua_handle(struct lua *l, struct weenet_process *p, struct weenet_message *m) {
	(void)p;
	lua_State *L = l->L;

	// traceback
	luaL_checktype(L, 1, LUA_TFUNCTION);

	lua_rawgetp(L, LUA_REGISTRYINDEX, _callback);

	lua_pushunsigned(L, (lua_Unsigned)m->source);
	lua_pushunsigned(L, (lua_Unsigned)m->session);
	lua_pushunsigned(L, (lua_Unsigned)m->tags.kind);
	lua_pushunsigned(L, (lua_Unsigned)m->tags.code);
	lua_pushlightuserdata(L, (void*)m->data);
	lua_pushlightuserdata(L, (void*)m->meta);

	int err = lua_pcall(L, 6, 0, 1);
	switch (err) {
	case LUA_OK:
		break;
	default:
		weenet_logger_errorf("lua service[%s file(%s) pid(%u)] message[kind(%d) code(%d) source(%d) session(%u) error: %s",
			l->name, l->filepath, l->self, m->tags.kind, m->tags.code, m->source, m->session, lua_tostring(L, -1));
		lua_pop(L, 1);
		break;
	}

	return 0;
}

static void
lua_delete(struct lua *l) {
	lua_close(l->L);
}

const struct weenet_interface lua_service = {
	.new	= (service_new_t)lua_new,
	.handle = (service_handle_t)lua_handle,
	.delete = (service_delete_t)lua_delete,
};
