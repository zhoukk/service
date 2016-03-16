#include "env.h"
#include "lock.h"
#include "service.h"

#include "lua.h"
#include "lauxlib.h"

struct env {
	struct spinlock lock;
	lua_State *L;
};

struct env *
env_create(const char *file) {
	struct env *env = (struct env *)service_alloc(0, sizeof *env);
	spinlock_init(&env->lock);
	env->L = luaL_newstate();
	if (file && LUA_OK != luaL_dofile(env->L, file)) {
		fprintf(stderr, "env init failed, %s\n", lua_tostring(env->L, -1));
		lua_close(env->L);
		spinlock_unit(&env->lock);
		service_alloc(env, 0);
		return 0;
	}
	return env;
}

void
env_release(struct env *env) {
	lua_close(env->L);
	spinlock_unit(&env->lock);
	service_alloc(env, 0);
}

void
env_setint(struct env * env, const char *key, int val) {
	spinlock_lock(&env->lock);
	lua_getglobal(env->L, key);
	lua_pop(env->L, 1);
	lua_pushinteger(env->L, val);
	lua_setglobal(env->L, key);
	spinlock_unlock(&env->lock);
}

int
env_getint(struct env * env, const char *key) {
	int val;
	spinlock_lock(&env->lock);
	lua_getglobal(env->L, key);
	val = lua_tointeger(env->L, -1);
	lua_pop(env->L, 1);
	spinlock_unlock(&env->lock);
	return val;
}

void
env_setstr(struct env * env, const char *key, const char *val) {
	spinlock_lock(&env->lock);
	lua_getglobal(env->L, key);
	lua_pop(env->L, 1);
	lua_pushstring(env->L, val);
	lua_setglobal(env->L, key);
	spinlock_unlock(&env->lock);
}

const char *
env_getstr(struct env * env, const char *key) {
	const char *val;
	spinlock_lock(&env->lock);
	lua_getglobal(env->L, key);
	val = lua_tostring(env->L, -1);
	lua_pop(env->L, 1);
	spinlock_unlock(&env->lock);
	return val;
}
