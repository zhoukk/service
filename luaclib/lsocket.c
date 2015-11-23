#include "socket.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>

void *lsocket_alloc(void *p, int size) {
	if (0 == size) {
		free(p);
		return 0;
	}
	return malloc(size);
}

struct buffer_node {
	char *data;
	int size;
	struct buffer_node *next;
};

struct buffer {
	int size;
	int offset;
	struct buffer_node *head;
	struct buffer_node *tail;
};

struct bufferpool {
	struct buffer_node *pool;
	struct buffer_node *freelist;
};

static void _freebuffer(struct buffer *b, struct bufferpool *bp) {
	if (b->head) {
		struct buffer_node *node = b->head;
		if (!node->next) {
			assert(b->tail == node);
			b->head = b->tail = 0;
		} else {
			b->head = node->next;
		}
		b->offset = 0;
		lsocket_alloc(node->data, 0);
		node->data = 0;
		node->size = 0;
		node->next = bp->freelist;
		bp->freelist = node;
	}
}

static int lfreebufferpool(lua_State *L) {
	struct bufferpool *bp = (struct bufferpool *)lua_touserdata(L, 1);
	lsocket_alloc(bp->pool, 0);
	return 0;
}

static void lpop_string(lua_State *L, struct buffer *b, struct bufferpool *bp, int size, int skip) {
	luaL_Buffer lb;
	struct buffer_node *cur = b->head;
	if (size < cur->size - b->offset) {
		lua_pushlstring(L, cur->data + b->offset, size - skip);
		b->offset += size;
		return;
	}
	if (size == cur->size - b->offset) {
		lua_pushlstring(L, cur->data + b->offset, size - skip);
		_freebuffer(b, bp);
		return;
	}
	luaL_buffinit(L, &lb);
	for (;;) {
		int bytes = cur->size - b->offset;
		if (bytes >= size) {
			if (size > skip) {
				luaL_addlstring(&lb, cur->data + b->offset, size - skip);
			}
			b->offset += size;
			if (bytes == size) {
				_freebuffer(b, bp);
			}
			break;
		} else {
			int real_size = size - skip;
			if (real_size > 0) {
				luaL_addlstring(&lb, cur->data + b->offset, (real_size < bytes) ? real_size : bytes);
			}
			_freebuffer(b, bp);
			size -= bytes;
			if (size == 0) {
				break;
			}
			cur = b->head;
			assert(cur);
		}
	}
	luaL_pushresult(&lb);
}

static int lreadall(lua_State *L) {
	luaL_Buffer lb;
	struct bufferpool *bp = (struct bufferpool *)lua_touserdata(L, lua_upvalueindex(1));
	struct buffer *b = (struct buffer *)lua_touserdata(L, 1);
	if (!b) {
		return luaL_error(L, "need a buffer");
	}
	luaL_buffinit(L, &lb);
	while (b->head) {
		struct buffer_node *cur = b->head;
		luaL_addlstring(&lb, cur->data + b->offset, cur->size - b->offset);
		_freebuffer(b, bp);
	}
	b->size = 0;
	luaL_pushresult(&lb);
	return 1;
}

static int check_sep(struct buffer_node *node, int from, const char *sep, int seplen) {
	for (;;) {
		int size = node->size - from;
		if (size >= seplen) {
			return memcmp(node->data + from, sep, seplen) == 0;
		}
		if (size > 0) {
			if (memcmp(node->data + from, sep, size)) {
				return 0;
			}
		}
		node = node->next;
		sep += size;
		seplen -= size;
		from = 0;
	}
}

static int lreadline(lua_State *L) {
	struct buffer_node *cur;
	int i, from, bytes, len;
	size_t seplen;
	struct bufferpool *bp = (struct bufferpool *)lua_touserdata(L, lua_upvalueindex(1));
	struct buffer *b = (struct buffer *)lua_touserdata(L, 1);
	const char *sep = luaL_checklstring(L, 2, &seplen);
	int check = (int)luaL_optnumber(L, 3, 0);
	len = seplen;
	cur = b->head;
	if (!cur) {
		return 0;
	}
	from = b->offset;
	bytes = cur->size - from;
	for (i = 0; i <= b->size - len; i++) {
		if (check_sep(cur, from, sep, len)) {
			if (check) {
				lua_pushboolean(L, 1);
			} else {
				lpop_string(L, b, bp, i + len, len);
				b->size -= i + len;
			}
			return 1;
		}
		++from;
		--bytes;
		if (bytes == 0) {
			cur = cur->next;
			from = 0;
			if (!cur) {
				break;
			}
			bytes = cur->size;
		}
	}
	return 0;
}

static int lpopbuffer(lua_State *L) {
	struct bufferpool *bp = (struct bufferpool *)lua_touserdata(L, lua_upvalueindex(1));
	struct buffer *b = (struct buffer *)lua_touserdata(L, 1);
	int size = (int)luaL_checkinteger(L, 2);
	if (b->size < size || size == 0) {
		lua_pushnil(L);
	} else {
		lpop_string(L, b, bp, size, 0);
		b->size -= size;
	}
	lua_pushinteger(L, b->size);
	return 2;
}

static int lpushbuffer(lua_State *L) {
	struct bufferpool *bp = (struct bufferpool *)lua_touserdata(L, lua_upvalueindex(1));
	struct buffer *b = (struct buffer *)lua_touserdata(L, 1);
	char *data = (char *)lua_touserdata(L, 2);
	int size = (int)luaL_checkinteger(L, 3);
	struct buffer_node *node = bp->freelist;
	if (!node) {
		int i;
		int size = 2;
		bp->pool = (struct buffer_node *)lsocket_alloc(0, size*sizeof(struct buffer_node));
		for (i = 0; i < size - 1; i++) {
			bp->pool[i].next = &bp->pool[i + 1];
		}
		bp->pool[size - 1].next = 0;
		bp->freelist = &bp->pool[0];
		node = bp->freelist;
	}
	bp->freelist = node->next;
	node->data = data;
	node->size = size;
	node->next = 0;
	if (!b->head) {
		assert(b->tail == 0);
		b->head = b->tail = node;
	} else {
		b->tail->next = node;
		b->tail = node;
	}
	b->size += size;
	lua_pushinteger(L, b->size);
	return 1;
}

static int lfreebuffer(lua_State *L) {
	struct bufferpool *bp = (struct bufferpool *)lua_touserdata(L, lua_upvalueindex(1));
	struct buffer *b = (struct buffer *)lua_touserdata(L, 1);
	_freebuffer(b, bp);
	return 0;
}

static int lnewbuffer(lua_State *L) {
	struct buffer *b = (struct buffer *)lua_newuserdata(L, sizeof *b);
	memset(b, 0, sizeof *b);
	if (luaL_newmetatable(L, "buffers")) {
		luaL_Reg l[] = {
			{"push", lpushbuffer},
			{"pop", lpopbuffer},
			{"readall", lreadall},
			{"readline", lreadline},
			{"free", lfreebuffer},
			{0, 0},
		};
		struct bufferpool *bp = (struct bufferpool *)lua_touserdata(L, lua_upvalueindex(1));
		luaL_newlibtable(L, l);
		lua_pushlightuserdata(L, bp);
		luaL_setfuncs(L, l, 1);
		lua_setfield(L, -2, "__index");
	}
	lua_setmetatable(L, -2);
	return 1;
}

static int llisten(lua_State *L) {
	uint32_t handle = (uint32_t)lua_tointeger(L, lua_upvalueindex(1));
	const char *host = luaL_checkstring(L, 1);
	int port = (int)luaL_checkinteger(L, 2);
	int id = socket_listen(host, port, (void *)(intptr_t)handle);
	lua_pushinteger(L, id);
	return 1;
}

static int lopen(lua_State *L) {
	uint32_t handle = (uint32_t)lua_tointeger(L, lua_upvalueindex(1));
	const char *host = luaL_checkstring(L, 1);
	int port = (int)luaL_checkinteger(L, 2);
	int id = socket_open(host, port, (void *)(intptr_t)handle);
	lua_pushinteger(L, id);
	return 1;
}

static int lbind(lua_State *L) {
	uint32_t handle = (uint32_t)lua_tointeger(L, lua_upvalueindex(1));
	int fd = (int)luaL_checkinteger(L, 1);
	int id = socket_bind(fd, (void *)(intptr_t)handle);
	lua_pushinteger(L, id);
	return 1;
}

static int lstart(lua_State *L) {
	uint32_t handle = (uint32_t)lua_tointeger(L, lua_upvalueindex(1));
	int id = (int)luaL_checkinteger(L, 1);
	socket_start(id, (void *)(intptr_t)handle);
	return 0;
}

static int lclose(lua_State *L) {
	uint32_t handle = (uint32_t)lua_tointeger(L, lua_upvalueindex(1));
	int id = (int)luaL_checkinteger(L, 1);
	socket_close(id, (void *)(intptr_t)handle);
	return 0;
}

static int lsend(lua_State *L) {
	int id = (int)luaL_checkinteger(L, 1);
	void *data;
	size_t size;
	int priority;
	long nsend;
	if (lua_isuserdata(L, 2)) {
		data = lua_touserdata(L, 2);
		size = (int)luaL_checkinteger(L, 3);
		priority = (int)luaL_optinteger(L, 4, 0);
	} else {
		const char *p = luaL_checklstring(L, 2, &size);
		data = lsocket_alloc(0, size);
		memcpy(data, p, size);
		priority = (int)luaL_optinteger(L, 3, 0);
	}
	nsend = socket_send(id, data, size, priority);
	lua_pushinteger(L, (int)nsend);
	return 1;
}

static int lnodelay(lua_State *L) {
	int id = (int)luaL_checkinteger(L, 1);
	socket_nodelay(id);
	return 0;
}

static int ludp(lua_State *L) {
	uint32_t handle = (uint32_t)lua_tointeger(L, lua_upvalueindex(1));
	int id;
	const char *host = lua_tostring(L, 1);
	int port = (int)lua_tointeger(L, 2);
	id = socket_udp(host, port, (void *)(intptr_t)handle);
	if (id < 0) {
		return luaL_error(L, "udp init failed");
	}
	lua_pushinteger(L, id);
	return 1;
}

static int ludp_open(lua_State *L) {
	int id = (int)lua_tointeger(L, 1);
	const char *host = lua_tostring(L, 2);
	int port = (int)lua_tointeger(L, 3);
	if (socket_udpopen(id, host, port)) {
		return luaL_error(L, "udp open failed");
	}
	return 0;
}

static int ludp_send(lua_State *L) {
	int id = (int)luaL_checkinteger(L, 1);
	const char *address = luaL_checkstring(L, 2);
	void *data;
	size_t size;
	long nsend;
	if (lua_isuserdata(L, 3)) {
		data = lua_touserdata(L, 3);
		size = (int)luaL_checkinteger(L, 4);
	} else {
		const char *p = luaL_checklstring(L, 3, &size);
		data = lsocket_alloc(0, size);
		memcpy(data, p, size);
	}
	nsend = socket_udpsend(id, address, data, size);
	lua_pushinteger(L, (int)nsend);
	return 1;
}

static int ludp_address(lua_State *L) {
	size_t size = 0;
	const uint8_t *addr = (const uint8_t *)luaL_checklstring(L, 1, &size);
	uint16_t port = 0;
	int family;
	char tmp[256];
	void *src = (void *)(addr + 3);
	memcpy(&port, addr + 1, sizeof(uint16_t));
	port = ntohs(port);
	if (size == 1 + 2 + 4) {
		family = AF_INET;
	} else {
		if (size != 1 + 2 + 16) {
			return luaL_error(L, "invalid udp address");
		}
		family = AF_INET6;
	}
	if (inet_ntop(family, src, tmp, sizeof(tmp)) == 0) {
		return luaL_error(L, "invalid udp address");
	}
	lua_pushstring(L, tmp);
	lua_pushinteger(L, port);
	return 2;
}

static int lunpack(lua_State *L) {
	struct socket_message *sm = (struct socket_message *)lua_touserdata(L, 1);
	luaL_checkinteger(L, 2);
	lua_pushinteger(L, sm->type);
	lua_pushinteger(L, sm->id);
	lua_pushinteger(L, sm->size);
	if (sm->type == SOCKET_OPEN || sm->type == SOCKET_ACCEPT || sm->type == SOCKET_ERR) {
		lua_pushlstring(L, sm->data, strlen(sm->data));
	} else {
		lua_pushlightuserdata(L, sm->data);
	}
	if (sm->type == SOCKET_UDP) {
		int address_size = 0;
		const char *address = socket_udpaddress(sm, &address_size);
		if (address) {
			lua_pushlstring(L, address, address_size);
			return 5;
		}
	}
	return 4;
}

int luaopen_socket_c(lua_State *L) {
	struct bufferpool *bp;
	luaL_Reg l1[] = {
		{"buffer", lnewbuffer},
		{0, 0},
	};
	luaL_Reg l2[] = {
		{"listen", llisten},
		{"open", lopen},
		{"bind", lbind},
		{"start", lstart},
		{"close", lclose},
		{"send", lsend},
		{"nodelay", lnodelay},
		{"udp", ludp},
		{"udp_open", ludp_open},
		{"udp_send", ludp_send},
		{"udp_address", ludp_address},
		{"unpack", lunpack},
		{0, 0},
	};
	luaL_newlibtable(L, l1);
	bp = (struct bufferpool *)lua_newuserdata(L, sizeof *bp);
	bp->freelist = 0;
	bp->pool = 0;
	if (luaL_newmetatable(L, "bufferpool")) {
		lua_pushcfunction(L, lfreebufferpool);
		lua_setfield(L, -2, "__gc");
	}
	lua_setmetatable(L, -2);
	luaL_setfuncs(L, l1, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "handle");
	luaL_setfuncs(L, l2, 1);
	return 1;
}
