#include "lserial.h"

#include "lualib.h"
#include "lauxlib.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TYPE_NIL 0
#define TYPE_BOOL 1
#define TYPE_INTEGER 2
#define TYPE_NUMBER 3
#define TYPE_STRING 4
#define TYPE_USERDATA 5
#define TYPE_TABLE 6

#define DATA_SIZE 128
#define MAX_DEPTH 32

struct buffer_node {
	struct buffer_node *next;
	char data[DATA_SIZE];
};

struct buffer {
	struct buffer_node *head;
	struct buffer_node *cur;
	int size;
	int offset;
	char *data;
};

static void buffer_init(struct buffer *b, struct buffer_node *node) {
	b->head = node;
	assert(node->next == 0);
	b->size = 0;
	b->cur = b->head;
	b->offset = 0;
	b->data = 0;
}

static void buffer_unit(struct buffer *b) {
	struct buffer_node *node = b->head;
	node = node->next;
	while (node) {
		struct buffer_node *next = node->next;
		free(node);
		node = next;
	}
	b->head = 0;
	b->cur = 0;
	b->size = 0;
	b->offset = 0;
	b->data = 0;
}

static inline struct buffer_node *buffer_node_new(void) {
	struct buffer_node *node = (struct buffer_node *)malloc(sizeof(struct buffer_node));
	node->next = 0;
	return node;
}

static inline void buffer_push(struct buffer *b, char *data, int size) {
	if (b->offset == DATA_SIZE) {
_again:
		b->cur = b->cur->next = buffer_node_new();
		b->offset = 0;
	}
	if (b->offset <= DATA_SIZE - size) {
		memcpy(b->cur->data+b->offset, data, size);
		b->offset += size;
		b->size += size;
	} else {
		int _size = DATA_SIZE-b->offset;
		memcpy(b->cur->data+b->offset, data, _size);
		data += _size;
		b->size += _size;
		size -= _size;
		goto _again;
	}
}

static inline void buffer_push_nil(struct buffer *b) {
	uint8_t type = TYPE_NIL;
	buffer_push(b, (char *)&type, 1);
}

static inline void buffer_push_integer(struct buffer *b, lua_Integer v) {
	uint8_t type = TYPE_INTEGER;
	buffer_push(b, (char *)&type, 1);
	buffer_push(b, (char *)&v, sizeof v);
}

static inline void buffer_push_number(struct buffer *b, lua_Number v) {
	uint8_t type = TYPE_NUMBER;
	buffer_push(b, (char *)&type, 1);
	buffer_push(b, (char *)&v, sizeof v);
}

static inline void buffer_push_boolean(struct buffer *b, int v) {
	uint8_t type = TYPE_BOOL;
	buffer_push(b, (char *)&type, 1);
	buffer_push(b, (char *)&v, 1);
}

static inline void buffer_push_string(struct buffer *b, const char *str, int size) {
	uint8_t type = TYPE_STRING;
	buffer_push(b, (char *)&type, 1);
	buffer_push(b, (char *)&size, 4);
	buffer_push(b, (char *)str, size);
}

static inline void buffer_push_pointer(struct buffer *b, void *v) {
	uint8_t type = TYPE_USERDATA;
	buffer_push(b, (char *)&type, 1);
	buffer_push(b, (char *)&v, sizeof v);
}

static void lpack_one(lua_State *L, struct buffer *b, int idx, int depth);

static int buffer_push_array(lua_State *L, struct buffer *b, int idx, int depth) {
	int i;
	int array_size = lua_rawlen(L, idx);
	lua_Integer v = array_size;
	uint8_t type = TYPE_TABLE;
	buffer_push(b, (char *)&type, 1);
	buffer_push(b, (char *)&v, sizeof v);
	for (i=1; i<=array_size; i++) {
		lua_rawgeti(L, idx, i);
		lpack_one(L, b, -1, depth);
		lua_pop(L, 1);
	}
	return array_size;
}

static void buffer_push_hash(lua_State *L, struct buffer *b, int idx, int depth, int array_size) {
	lua_pushnil(L);
	while (lua_next(L, idx) != 0) {
		if (lua_type(L, -2) == LUA_TNUMBER)	{
			if (lua_isinteger(L, -2)) {
				lua_Integer x = lua_tointeger(L, -2);
				if (x > 0 && x <= array_size) {
					lua_pop(L, 1);
					continue;
				}
			}
		}
		lpack_one(L, b, -2, depth);
		lpack_one(L, b, -1, depth);
		lua_pop(L, 1);
	}
	buffer_push_nil(b);
}

static void buffer_push_metapairs(lua_State *L, struct buffer *b, int idx, int depth) {
	uint8_t type = TYPE_TABLE;
	lua_Integer v = 0;
	buffer_push(b, (char *)&type, 1);
	buffer_push(b, (char *)&v, sizeof v);
	lua_pushvalue(L, idx);
	lua_call(L, 1, 3);
	for (;;) {
		lua_pushvalue(L, -2);
		lua_pushvalue(L, -2);
		lua_copy(L, -5, -3);
		lua_call(L, 2, 2);
		if (lua_type(L, -2) == LUA_TNIL) {
			lua_pop(L, 4);
			break;
		}
		lpack_one(L, b, -2, depth);
		lpack_one(L, b, -1, depth);
		lua_pop(L, 1);
	}
	buffer_push_nil(b);
}

static void buffer_push_table(lua_State *L, struct buffer *b, int idx, int depth) {
	luaL_checkstack(L, LUA_MINSTACK, 0);
	if (idx < 0)
		idx = lua_gettop(L)+idx+1;
	if (luaL_getmetafield(L, idx, "__pairs") != LUA_TNIL)
		buffer_push_metapairs(L, b, idx, depth);
	else {
		int array_size = buffer_push_array(L, b, idx, depth);
		buffer_push_hash(L, b, idx, depth, array_size);
	}
}

static void lpack_one(lua_State *L, struct buffer *b, int idx, int depth) {
	int type;
	if (depth > MAX_DEPTH) {
		buffer_unit(b);
		luaL_error(L, "serial table too depth");
	}
	type = lua_type(L, idx);
	switch (type) {
		case LUA_TNIL:
			buffer_push_nil(b);
			break;
		case LUA_TNUMBER:
			if (lua_isinteger(L, idx))
				buffer_push_integer(b, lua_tointeger(L, idx));
			else
				buffer_push_number(b, lua_tonumber(L, idx));
			break;
		case LUA_TBOOLEAN:
			buffer_push_boolean(b, lua_toboolean(L, idx));
			break;
		case LUA_TSTRING: {
				size_t size;
				const char *p = lua_tolstring(L, idx, &size);
				buffer_push_string(b, p, size);
			}
			break;
		case LUA_TLIGHTUSERDATA:
			buffer_push_pointer(b, lua_touserdata(L, idx));
			break;
		case LUA_TTABLE:
			buffer_push_table(L, b, idx, depth+1);
			break;
		default:
			buffer_unit(b);
			luaL_error(L, "serial unsupport type %s", lua_typename(L, type));
	}
}

static void lpack_ret(lua_State *L, struct buffer_node *node, int len) {
	uint8_t *data = (uint8_t *)malloc(len);
	uint8_t *p = data;
	int size = len;
	while (len > 0) {
		if (len >= DATA_SIZE) {
			memcpy(p, node->data, DATA_SIZE);
			p += DATA_SIZE;
			len -= DATA_SIZE;
			node = node->next;
		} else {
			memcpy(p, node->data, len);
			break;
		}
	}
	lua_pushlightuserdata(L, data);
	lua_pushinteger(L, size);
}

int lserial_pack(lua_State *L) {
	struct buffer_node node = {0};
	struct buffer b;
	int i, n = lua_gettop(L);
	buffer_init(&b, &node);
	for (i=1; i<=n; i++)
		lpack_one(L, &b, i, 0);
	assert(b.head == &node);
	lpack_ret(L, &node, b.size);
	buffer_unit(&b);
	return 2;
}

static void *buffer_pop(struct buffer *b, int size) {
	int offset;
	if (b->size < size)
		return 0;
	offset = b->offset;
	b->offset += size;
	b->size -= size;
	return b->data + offset;
}

static inline void buffer_pop_nil(lua_State *L) {
	lua_pushnil(L);
}

static inline void buffer_pop_bool(lua_State *L, struct buffer *b) {
	char *v = (char *)buffer_pop(b, 1);
	lua_pushboolean(L, (int)*v);
}

static inline void buffer_pop_integer(lua_State *L, struct buffer *b) {
	lua_Integer *v = (lua_Integer *)buffer_pop(b, sizeof(lua_Integer));
	lua_pushinteger(L, *v);
}

static inline void buffer_pop_number(lua_State *L, struct buffer *b) {
	lua_Number *v = (lua_Number *)buffer_pop(b, sizeof(lua_Number));
	lua_pushnumber(L, *v);
}

static inline void buffer_pop_string(lua_State *L, struct buffer *b) {
	int size = *(int *)buffer_pop(b, 4);
	char *p = (char *)buffer_pop(b, size);
	lua_pushlstring(L, p, size);
}

static inline void buffer_pop_userdata(lua_State *L, struct buffer *b) {
	void **v = (void **)buffer_pop(b, sizeof(void *));
	if (!v) {
		lua_pushnil(L);
		return;
	}
	lua_pushlightuserdata(L, *v);
}

static void lunpack_value(lua_State *L, struct buffer *b, int type);

static void lunpack_one(lua_State *L, struct buffer *b) {
	uint8_t type;
	uint8_t *t = (uint8_t *)buffer_pop(b, 1);
	if (!t)	return;
	type = *t;
	lunpack_value(L, b, type);
}

static void buffer_pop_table(lua_State *L, struct buffer *b) {
	lua_Integer *v = (lua_Integer *)buffer_pop(b, sizeof(lua_Integer));
	int array_size = (int)*v;
	int i;
	luaL_checkstack(L, LUA_MINSTACK, 0);
	lua_createtable(L, array_size, 0);
	for (i=1; i<=array_size; i++) {
		lunpack_one(L, b);
		lua_rawseti(L, -2, i);
	}
	for (;;) {
		lunpack_one(L, b);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			return;
		}
		lunpack_one(L, b);
		lua_rawset(L, -3);
	}
}

static void lunpack_value(lua_State *L, struct buffer *b, int type) {
	switch (type) {
		case TYPE_NIL:
			buffer_pop_nil(L);
			break;
		case TYPE_BOOL:
			buffer_pop_bool(L, b);
			break;
		case TYPE_INTEGER:
			buffer_pop_integer(L, b);
			break;
		case TYPE_NUMBER:
			buffer_pop_number(L, b);
			break;
		case TYPE_STRING:
			buffer_pop_string(L, b);
			break;
		case TYPE_USERDATA:
			buffer_pop_userdata(L, b);
			break;
		case TYPE_TABLE:
			buffer_pop_table(L, b);
			break;
		default:
			break;
	}
}

int lserial_unpack(lua_State *L) {
	if (lua_isnoneornil(L, 1)) {
		return 0;
	} else {
		void *data;
		int size;
		int i;
		struct buffer b;
		if (lua_type(L, 1) == LUA_TSTRING) {
			size_t _size;
			data = (void *)lua_tolstring(L, 1, &_size);
			size = (int)_size;
		} else {
			data = lua_touserdata(L, 1);
			size = (int)luaL_checkinteger(L, 2);
		}
		if (size == 0 || data == 0) return 0;
		b.offset = 0;
		b.data = (char *)data;
		b.size = size;
		lua_settop(L, 0);
		for (i=0; ;i++) {
			uint8_t type;
			uint8_t *t;
			if (i%8==7)
				luaL_checkstack(L, LUA_MINSTACK, 0);
			t = (uint8_t *)buffer_pop(&b, 1);
			if (!t) break;
			type = *t;
			lunpack_value(L, &b, type);
		}
		return lua_gettop(L);
	}
}
