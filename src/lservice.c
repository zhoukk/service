#include "service.h"
#include "timer.h"
#include "lserial.h"
#include "lalloc.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int service_serial(lua_State *L) {
	luaL_Reg l[] = {
		{ "pack", lserial_pack },
		{ "unpack", lserial_unpack },
		{ 0, 0 },
	};
	luaL_newlib(L, l);
	return 1;
}

static int ltraceback(lua_State *L) {
	const char *err = lua_tostring(L, 1);
	if (err) {
		luaL_traceback(L, L, err, 1);
	} else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}

struct service_lua {
	lua_State *L;
	struct allocator *A;
};

static int lservice_dispatch(uint32_t handle, void *ud, const struct message *m) {
	struct service_lua *sl = (struct service_lua *)ud;
	lua_State *L = sl->L;
	int top = lua_gettop(L);
	if (top == 0) {
		lua_pushcfunction(L, ltraceback);
		lua_rawgetp(L, LUA_REGISTRYINDEX, lservice_dispatch);
	} else {
		assert(top == 2);
	}
	lua_pushvalue(L, 2);
	lua_pushinteger(L, m->proto);
	lua_pushinteger(L, m->session);
	lua_pushinteger(L, m->source);
	lua_pushlightuserdata(L, m->data);
	lua_pushinteger(L, m->size);
	int r = lua_pcall(L, 5, 0, 1);
	if (r == LUA_OK) {
		return 0;
	}
	switch (r) {
	case LUA_ERRRUN:
		service_log(handle, "LUA_ERRRUN :%s\n", lua_tostring(L, -1));
		break;
	case LUA_ERRMEM:
		service_log(handle, "LUA_ERRMEM\n");
		break;
	case LUA_ERRERR:
		service_log(handle, "LUA_ERRERR :%s\n", lua_tostring(L, -1));
		break;
	case LUA_ERRGCMM:
		service_log(handle, "LUA_ERRGCMM\n");
		break;
	}
	lua_pop(L, 1);
	return 0;
}

int service_c(lua_State *L);

static void *lservice_create(uint32_t handle, const char *param) {
	struct service_lua *sl = service_alloc(0, sizeof *sl);
	sl->A = allocator_new();
	sl->L = lua_newstate(lalloc, sl->A);
	lua_State *L = sl->L;
	lua_gc(L, LUA_GCSTOP, 0);
	luaL_openlibs(L);
	lua_pushinteger(L, handle);
	lua_setfield(L, LUA_REGISTRYINDEX, "handle");
	luaL_requiref(L, "service.c", service_c, 0);
	luaL_requiref(L, "service.serial", service_serial, 0);
	lua_settop(L, 0);
	const char *path = service_env_get("lua_path");
	lua_pushstring(L, path);
	lua_setglobal(L, "LUA_PATH");
	const char *cpath = service_env_get("lua_cpath");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");
	const char *spath = service_env_get("service_path");
	lua_pushstring(L, spath);
	lua_setglobal(L, "SERVICE_PATH");
	lua_pushcfunction(L, ltraceback);
	assert(lua_gettop(L) == 1);
	const char *loader = service_env_get("loader");
	if (LUA_OK != luaL_loadfile(L, loader)) {
		service_log(handle, "%s\n", lua_tostring(L, -1));
		lua_close(L);
		return 0;
	}
	lua_pushstring(L, param);
	if (LUA_OK != lua_pcall(L, 1, 0, 1)) {
		service_log(handle, "%s\n", lua_tostring(L, -1));
		lua_close(L);
		return 0;
	}
	lua_settop(L, 0);
	lua_gc(L, LUA_GCRESTART, 0);
	return (void *)sl;
}

static void lservice_release(uint32_t handle, void *ud) {
	struct service_lua *sl = (struct service_lua *)ud;
	lua_close(sl->L);
	allocator_free(sl->A);
	service_alloc(sl, 0);
}

struct module lua_mod = {
	lservice_dispatch,
	lservice_create,
	lservice_release,
};

static int lservice(lua_State *L) {
	const char *param = luaL_checkstring(L, 1);
	uint32_t handle = service_create(&lua_mod, param);
	lua_pushinteger(L, handle);
	return 1;
}

static int lexit(lua_State *L) {
	uint32_t handle;
	if (lua_isinteger(L, 1)) {
		handle = (uint32_t)luaL_checkinteger(L, 1);
	} else {
		lua_getfield(L, LUA_REGISTRYINDEX, "handle");
		handle = (uint32_t)luaL_checkinteger(L, -1);
	}
	service_release(handle);
	return 0;
}

static int lsend(lua_State *L) {
	struct message m;
	uint32_t handle = (uint32_t)luaL_checkinteger(L, 1);
	m.proto = (int)luaL_checkinteger(L, 2);
	m.session = (int)luaL_checkinteger(L, 3);
	if (lua_isuserdata(L, 4)) {
		m.data = lua_touserdata(L, 4);
		m.size = (int)luaL_checkinteger(L, 5);
	} else {
		size_t size;
		const char *msg = lua_tolstring(L, 4, &size);
		if (size > 0) {
			m.data = service_alloc(0, size);
			memcpy(m.data, msg, size);
			m.size = (int)size;
		} else {
			m.data = 0;
			m.size = 0;
		}
	}
	lua_getfield(L, LUA_REGISTRYINDEX, "handle");
	m.source = (uint32_t)luaL_checkinteger(L, -1);
	lua_pushinteger(L, service_send(handle, &m));
	return 1;
}

static int lstart(lua_State *L) {
	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, lservice_dispatch);
	lua_getfield(L, LUA_REGISTRYINDEX, "handle");
	return 1;
}

static int lsession(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, "handle");
	uint32_t handle = (uint32_t)luaL_checkinteger(L, -1);
	lua_pushinteger(L, service_session(handle));
	return 1;
}

static int lnowtime(lua_State *L) {
	lua_pushinteger(L, timer_now());
	return 1;
}

static int lstarttime(lua_State *L) {
	lua_pushinteger(L, timer_starttime());
	return 1;
}

static int ltimeout(lua_State *L) {
	int ti = (int)luaL_checkinteger(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "handle");
	uint32_t handle = (uint32_t)luaL_checkinteger(L, -1);
	int session = service_timeout(handle, ti);
	lua_pushinteger(L, session);
	return 1;
}

static int ltrash(lua_State *L) {
	void *data = lua_touserdata(L, 1);
	service_alloc(data, 0);
	return 0;
}

static int ltostring(lua_State *L) {
	void *data = lua_touserdata(L, 1);
	int size = luaL_checkinteger(L, 2);
	lua_pushlstring(L, data, size);
	service_alloc(data, 0);
	return 1;
}

static int lname(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	uint32_t handle = (uint32_t)luaL_checkinteger(L, 2);
	service_name(name, handle);
	return 0;
}

static int lquery(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	uint32_t handle = service_query(name);
	if (handle > 0) {
		lua_pushinteger(L, handle);
		return 1;
	}
	return 0;
}

static int llog(lua_State *L) {
	const char *log = luaL_checkstring(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "handle");
	uint32_t handle = (uint32_t)luaL_checkinteger(L, -1);
	service_log(handle, log);
	return 0;
}

static int lmqlen(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, "handle");
	uint32_t handle = (uint32_t)luaL_checkinteger(L, -1);
	int mqlen = service_mqlen(handle);
	lua_pushinteger(L, mqlen);
	return 1;
}

static int lgetenv(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	const char *val = service_env_get(key);
	lua_pushstring(L, val);
	return 1;
}

static int lsetenv(lua_State *L) {
	const char *key = luaL_checkstring(L, 1);
	const char *val = luaL_checkstring(L, 2);
	service_env_set(key, val);
	return 0;
}

static int llogon(lua_State *L) {
	uint32_t handle = (uint32_t)luaL_checkinteger(L, 1);
	service_logon(handle);
	return 0;
}

static int llogoff(lua_State *L) {
	uint32_t handle = (uint32_t)luaL_checkinteger(L, 1);
	service_logoff(handle);
	return 0;
}

static int labort(lua_State *L) {
	service_abort();
	return 0;
}

int service_c(lua_State *L) {
	luaL_Reg l[] = {
		{"service", lservice},
		{"exit", lexit},
		{"send", lsend},
		{"start", lstart},
		{"session", lsession},
		{"now", lnowtime},
		{"starttime", lstarttime},
		{"timeout", ltimeout},
		{"trash", ltrash},
		{"tostring", ltostring},
		{"name", lname},
		{"query", lquery},
		{"log", llog},
		{"mqlen", lmqlen},
		{"getenv", lgetenv},
		{"setenv", lsetenv},
		{"logon", llogon},
		{"logoff", llogoff},
		{"abort", labort},
		{0, 0},
	};
	luaL_newlib(L, l);
	return 1;
}
