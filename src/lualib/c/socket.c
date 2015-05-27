#include "event.h"
#include "logger.h"
#include "process.h"
#include "utils.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#undef EEOF
#define EEOF	(-1)

static size_t
_read(int fd, byte_t *buf, size_t len, int *err) {
	size_t n = 0;
again:
	for (;;) {
		ssize_t rd = read(fd, buf, len);
		if (rd == 0) {
			*err = EEOF;
			break;
		}
		if (rd < 0) {
			if (errno == EINTR) goto again;	// bypass condition check
			*err = errno;
			break;
		}
		size_t rz = (size_t)rd;
		n += rz;
		len -= rz;
		if (len == 0) break;
		buf += rz;
	}
	return n;
}

static size_t
_write(int fd, byte_t *data, size_t size, int *err) {
	size_t n = 0;
again:
	for (;;) {
		ssize_t wr = write(fd, (void*)data, size);
		if (wr < 0) {
			if (errno == EINTR) goto again;
			*err = errno;
			break;
		}
		size_t wz = (size_t)wr;
		n += wz;
		size -= wz;
		if (size == 0) break;
		data += wz;
	}
	return n;
}

static int
_noblocking(int fd) {
	int nb = 1;
	return ioctl(fd, FIONBIO, &nb);
}

static int
_accept(int fd, int *err) {
	*err = 0;
	for (;;) {
		int conn = accept(fd, NULL, NULL);
		if (conn < 0) {
			switch (errno) {
			case EINTR:
				continue;
			default:
				*err = errno;
				return -1;
			}
		}
		if (_noblocking(conn) == -1) {
			weenet_logger_fatalf("set noblocking mode failed: %s\n", strerror(errno));
			close(conn);
			continue;
		}
		return conn;
	}
	return -1;
}

static struct weenet_process *
_get_process(lua_State *L) {
	return lua_touserdata(L, lua_upvalueindex(1));
}

static int
_yield_socket(lua_State *L, int fd, int event, lua_KContext ctx, lua_KFunction k) {
	struct weenet_process *p = _get_process(L);
	process_t self = weenet_process_pid(p);
	session_t session = weenet_process_sid(p);
	weenet_event_monitor(self, session, fd, WEVENT_ADD, event);
	lua_pushliteral(L, "REQUEST");
	lua_pushinteger(L, (lua_Integer)session);
	return lua_yieldk(L, 2, ctx, k);
}

// FIXME return value ?
static int
_check_socket_result(lua_State *L, int err, int fd, int event, lua_KContext ctx, lua_KFunction k) {
	switch (err) {
	case EAGAIN:
		break;
	case EEOF:
		lua_pushnil(L);
		lua_pushliteral(L, "peer closed");
		return 2;
	default:
		lua_pushnil(L);
		lua_pushstring(L, strerror(err));
		return 2;
	}
	return _yield_socket(L, fd, event, ctx, k);
}

static int
_socket_pipe(lua_State *L) {
	int fd[2];
	if (pipe2(fd, O_NONBLOCK|O_CLOEXEC) == -1) {
		lua_pushnil(L);
		lua_pushnil(L);
		lua_pushstring(L, strerror(errno));
		return 3;
	}
	lua_pushinteger(L, fd[0]);
	lua_pushinteger(L, fd[1]);
	return 2;
}

static int
_socket_readk(lua_State *L, int status, lua_KContext ctx) {
	int fd = (int)luaL_checkinteger(L, 1);
	size_t required = 0;
	if (status == LUA_OK) {
		required = (size_t)luaL_optinteger(L, 2, 0);
		lua_settop(L, 1);
		if (required != 0) {
			lua_newuserdata(L, required);
		}
	} else if (ctx >= 0) {
		required = lua_rawlen(L, 2);
	}
	lua_settop(L, 2);	// fd and possible userdata
	if (required == 0) {
		byte_t buf[1024];
		int err = 0;
		size_t n = _read(fd, buf, sizeof buf, &err);
		if (n == 0 || (err != 0 && err != EAGAIN)) {
			return _check_socket_result(L, err, fd, WEVENT_READ|WEVENT_ONESHOT, -1, _socket_readk);
		}
		lua_pushlstring(L, (char*)buf, (size_t)n);
		return 1;
	}
	byte_t *buf = lua_touserdata(L, 2);
	size_t n = (size_t)ctx;
	int err = 0;
	n += _read(fd, buf+n, required-n, &err);
	if (n == required) {
		lua_pushlstring(L, (char*)buf, required);
		return 1;
	}
	return _check_socket_result(L, err, fd, WEVENT_READ|WEVENT_ONESHOT, (int)n, _socket_readk);
}

static int
_socket_read(lua_State *L) {
	return _socket_readk(L, LUA_OK, 0);
}

static int
_socket_writek(lua_State *L, int status, lua_KContext ctx) {
	int fd = (int)luaL_checkinteger(L, 1);
	byte_t *data;
	size_t size;
	if (lua_isuserdata(L, 2)) {
		data = lua_touserdata(L, 2);
		size = lua_rawlen(L, 2);
	} else {
		data = (byte_t*)luaL_checklstring(L, 2, &size);
	}
	lua_settop(L, 2);

	if (size == 0) {
		lua_pushboolean(L, 1);
		return 1;
	}

	if (status == LUA_YIELD) {
		size_t n = (size_t)ctx;
		data += n;
		size -= n;
	}

	int err = 0;
	size_t n = _write(fd, data, size, &err);
	if (n == size) {
		lua_pushboolean(L, 1);
		return 1;
	}

	return _check_socket_result(L, err, fd, WEVENT_WRITE|WEVENT_ONESHOT, (lua_KContext)n + ctx, _socket_writek);
}

static int
_socket_write(lua_State *L) {
	return _socket_writek(L, LUA_OK, 0);
}

static int
_socket_close(lua_State *L) {
	int fd = (int)luaL_checkinteger(L, 1);
	if (close(fd) == -1 && errno == EBADF) {
		return luaL_error(L, "bad file descriptor");
	}
	return 0;
}

static int
_socket_accept(lua_State *L) {
	int fd = (int)luaL_checkinteger(L, 1);
	lua_settop(L, 1);
	int err;
	int conn = _accept(fd, &err);
	if (conn == -1) {
		if (err == EAGAIN) {
			return _check_socket_result(L, err, fd, WEVENT_READ|WEVENT_ONESHOT, 0, (lua_KFunction)_socket_accept);
		}
		lua_pushnil(L);
		lua_pushstring(L, strerror(err));
		return 2;
	}
	lua_pushinteger(L, conn);
	return 1;
}

#define TCP_PREFIX	"tcp://"
#define TCP4_PREFIX	"tcp4://"
#define TCP6_PREFIX	"tcp6://"

// ":6666"
// "*:6060"
// "tcp://*:6666"
// "tcp6://:6666"
// "tcp4://12.34.0.39:6666"
static size_t
_trim(char *addr, int *family) {
	*family = AF_UNSPEC;
	if (strncmp(addr, TCP_PREFIX, sizeof(TCP_PREFIX)-1) == 0) {
		return sizeof(TCP_PREFIX)-1;
	} else if (strncmp(addr, TCP4_PREFIX, sizeof(TCP4_PREFIX)-1) == 0) {
		*family = AF_INET;
		return sizeof(TCP4_PREFIX)-1;
	} else if (strncmp(addr, TCP6_PREFIX, sizeof(TCP6_PREFIX)-1) == 0) {
		*family = AF_INET6;
		return sizeof(TCP6_PREFIX)-1;
	}
	return 0;
}

static int
_parse(char *addr, size_t len, int *family, char **host, char **port) {
	size_t n = _trim(addr, family);
	addr += n;
	len -= n;

	char *sep = memrchr(addr, ':', len);
	if (sep == NULL) return -1;

	char *beg = NULL;
	char *end = NULL;
	if (addr[0] == '[') {
		end = sep-1;
		if (*end != ']') {
			return -1;
		}
		beg = addr+1;
	} else {
		beg = addr;
		end = sep;
	}
	assert(beg <= end);
	if ((beg == end) || ((beg+1 == end) && beg[0] == '*')) {
		*host = NULL;
	} else {
		*end = '\0';
		*host = beg;
	}
	*port = sep+1;
	return 0;
}

static int
_option(int fd) {
	if (_noblocking(fd) == -1) return -1;
	int reuse = 1;
	return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

static const char *
_getaddrinfo(int family, const char *host, const char *port, struct addrinfo **res, int flag) {
	struct addrinfo hint;
	memzero(&hint, sizeof hint);
	hint.ai_family = family;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_protocol = IPPROTO_TCP;
	hint.ai_flags = flag | AI_NUMERICHOST | AI_NUMERICSERV;

	*res = NULL;
	int err = getaddrinfo(host, port, &hint, res);
	switch (err) {
	case 0:
		break;
	case EAI_SYSTEM:
		return strerror(errno);
	default:
		return gai_strerror(err);
	}
	return (*res == NULL) ? "no match addrinfo" : NULL;
}

static const char *
_listen(const char *address, size_t len, int backlog, int * restrict nsocket, int sockets[]) {
	char copy[len+1];
	memcpy(copy, address, len);
	copy[len] = '\0';

	char *host = NULL;
	char *port = NULL;
	int family = AF_UNSPEC;
	int err = _parse(copy, len, &family, &host, &port);
	if (err != 0) {
		return "illegal address format";
	}

	struct addrinfo *res;
	const char *error = _getaddrinfo(family, host, port, &res, AI_PASSIVE);
	if (error != NULL) {
		return error;
	}

	int n = 0;
	for (struct addrinfo *ai = res; ai != NULL && n < *nsocket; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd != -1) {
			if (_option(fd) == 0
			 && bind(fd, ai->ai_addr, ai->ai_addrlen) == 0
			 && listen(fd, backlog) == 0) {
				sockets[n++] = fd;
				continue;
			}
			close(fd);
		}
	}
	freeaddrinfo(res);
	*nsocket = n;

	return n == 0 ? "failed to listen" : NULL;
}

static void
_closen(int n, int fds[]) {
	while (n--) {
		close(fds[n]);
	}
}

static int
_socket_listen(lua_State *L) {
	if (lua_gettop(L) >= 2) {
		lua_settop(L, 2);
		lua_pushstring(L, ":");
		lua_insert(L, 2);
		lua_concat(L, 3);
	}
	size_t len;
	const char *addr = luaL_checklstring(L, 1, &len);
	int nsocket = 0x10;
	int sockets[nsocket];
	const char *err = _listen(addr, len, SOMAXCONN, &nsocket, sockets);
	if (err) {
		lua_pushnil(L);
		lua_pushstring(L, err);
		return 2;
	}

	if (nsocket > 1) {
		lua_pushnil(L);
		lua_pushliteral(L, "ambiguous address lead to multiple fds, unsupported now");
		_closen(nsocket, sockets);
		return 2;
	}

	lua_pushinteger(L, (lua_Integer)sockets[0]);
	return 1;
}

static int
_getsockerr(int fd) {
	int err = 0;
	socklen_t len = sizeof(err);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
		err = errno;
	}
	return err;
}

static const char *
_getnameinfo(struct addrinfo *ai, char host[NI_MAXHOST], char port[NI_MAXSERV]) {
	int err = getnameinfo(ai->ai_addr, ai->ai_addrlen, host, NI_MAXHOST, port, NI_MAXSERV, NI_NUMERICHOST|NI_NUMERICSERV);
	switch (err) {
	case 0:
		break;
	case EAI_SYSTEM:
		return strerror(errno);
	default:
		return gai_strerror(err);
	}
	return NULL;
}

static void
_freeaddrinfo(lua_State *L) {
	struct addrinfo *res = lua_touserdata(L, 2);
	assert(res != NULL);
	freeaddrinfo(res);
}

static int _connectk(lua_State *L, int status, lua_KContext ctx);

static int
_connect(lua_State *L, struct addrinfo *ai, int nerrs) {
	char host[NI_MAXHOST], port[NI_MAXSERV];
	for (; ai; ai = ai->ai_next) {
		const char *err = _getnameinfo(ai, host, port);
		++nerrs;
		if (err) {
			lua_pushfstring(L, "\n\tgetnameinfo() failed: %s", err);
			continue;
		} else {
			lua_pushfstring(L, "\n\ttry address %s:%s : ", host, port);
		}

		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			++nerrs;
			lua_pushfstring(L, "\n\t\tsocket(%d, %d, %d) error: %s",
					ai->ai_family, ai->ai_socktype, ai->ai_protocol,
					strerror(errno));
			continue;
		}
		_noblocking(fd);
		int e = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (e != 0 && errno != EINPROGRESS) {
			++nerrs;
			lua_pushfstring(L, "\n\t\tconnect() error: %s", strerror(errno));
			close(fd);
			continue;
		}
		lua_pushlightuserdata(L, ai);
		lua_pushinteger(L, (lua_Integer)nerrs);
		lua_pushinteger(L, (lua_Integer)fd);
		return _yield_socket(L, fd, WEVENT_WRITE, lua_gettop(L), _connectk);
	}
	_freeaddrinfo(L);
	lua_pushliteral(L, "\n\tall attempts failed");
	lua_concat(L, nerrs+1);
	lua_pushnil(L);
	lua_pushvalue(L, -2);
	return 2;
}

static int
_connectk(lua_State *L, int status, lua_KContext ctx) {
	assert(status == LUA_YIELD);
	int top = (int)ctx;
	lua_settop(L, top);
	int fd = (int)lua_tointeger(L, -1);
	int nerrs = (int)lua_tointeger(L, -2);
	struct addrinfo *ai = lua_touserdata(L, -3);
	assert(ai != NULL);
	int err = _getsockerr(fd);
	if (err != 0) {
		close(fd);
		lua_pop(L, 3);
		lua_pushfstring(L, "\n\t\tsocket error: %s", strerror(err));
		return _connect(L, ai->ai_next, nerrs+1);
	}
	_freeaddrinfo(L);
	lua_pushinteger(L, (lua_Integer)fd);
	return 1;
}

static int
_socket_connect(lua_State *L) {
	if (lua_gettop(L) >= 2) {
		lua_settop(L, 2);
		lua_pushstring(L, ":");
		lua_insert(L, 2);
		lua_concat(L, 3);
	}
	assert(lua_gettop(L) == 1);
	size_t len;
	const char *addr = luaL_checklstring(L, 1, &len);

	char copy[len+1];
	memcpy(copy, addr, len);
	copy[len] = '\0';

	char *host = NULL;
	char *port = NULL;
	int family = AF_UNSPEC;
	int err = _parse(copy, len, &family, &host, &port);
	if (err != 0) {
		lua_pushnil(L);
		lua_pushliteral(L, "illegal address format");
		return 2;
	}

	struct addrinfo *res;
	const char *error = _getaddrinfo(family, host, port, &res, 0);
	if (error != NULL) {
		lua_pushnil(L);
		lua_pushstring(L, error);
		return 2;
	} 
	lua_pushlightuserdata(L, res);
	assert(lua_gettop(L) == 2);

	lua_pushfstring(L, "connecting to %s:", addr);
	return _connect(L, res, 1);
}

static const luaL_Reg socket_funcs[] = {
	{"pipe", _socket_pipe},
	{"read", _socket_read},
	{"write", _socket_write},
	{"close", _socket_close},
	{"accept", _socket_accept},
	{"listen", _socket_listen},
	{"connect", _socket_connect},
	{NULL, NULL},
};

int
luaopen_socket(lua_State *L) {
	luaL_newlibtable(L, socket_funcs);
	lua_rawgetp(L, LUA_REGISTRYINDEX, weenet_process_new);
	luaL_setfuncs(L, socket_funcs, 1);

	lua_pushinteger(L, 0);
	lua_setfield(L, -2, "stdin");

	lua_pushinteger(L, 1);
	lua_setfield(L, -2, "stdout");

	lua_pushinteger(L, 2);
	lua_setfield(L, -2, "stderr");

	return 1;
}
