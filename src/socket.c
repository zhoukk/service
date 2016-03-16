#include "socket.h"
#include "event.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <stddef.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define MAX_EVENT 64
#define MAX_SOCKET_P 16
#define MAX_SOCK_INFO 128
#define MIN_SOCK_BUFF 64
#define MAX_SOCKET (1<<MAX_SOCKET_P)

#define SOCKET_TYPE_INVALID 0
#define SOCKET_TYPE_RESERVE 1
#define SOCKET_TYPE_OPENING 2
#define SOCKET_TYPE_OPENED 3
#define SOCKET_TYPE_LISTEN 4
#define SOCKET_TYPE_PLISTEN 5
#define SOCKET_TYPE_PACCEPT 6
#define SOCKET_TYPE_BIND 7
#define SOCKET_TYPE_HALFCLOSE 8

#define SOCKET_REQ_OPEN 1
#define SOCKET_REQ_LISTEN 2
#define SOCKET_REQ_CLOSE 3
#define SOCKET_REQ_EXIT 4
#define SOCKET_REQ_BIND 5
#define SOCKET_REQ_START 6
#define SOCKET_REQ_SEND 7
#define SOCKET_REQ_OPT 8
#define SOCKET_REQ_UDP 9
#define SOCKET_REQ_SETUDP 10
#define SOCKET_REQ_SENDUDP 11

#define PROTOCOL_TCP 0
#define PROTOCOL_UDP 1
#define PROTOCOL_UDPv6 2

#define UDP_ADDRESS_SIZE 19
#define MAX_UDP_PACKAGE 65535

#define HASH_ID(id) (id%MAX_SOCKET)

#define atom_cas(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define atom_inc(ptr) __sync_add_and_fetch(ptr, 1)
#define atom_and(ptr, n) __sync_and_and_fetch(ptr, n)

struct buffer {
	struct buffer *next;
	char *buff;
	int len;
	char *ptr;
	int send_object;
	uint8_t udp_address[UDP_ADDRESS_SIZE];
};

struct buffer_list {
	struct buffer *head;
	struct buffer *tail;
};

#define SIZEOF_TCPBUFFER (offsetof(struct buffer, udp_address[0]))
#define SIZEOF_UDPBUFFER (sizeof(struct buffer))

struct socket {
	int fd;
	int id;
	void *ud;
	long wb_size;
	struct buffer_list high;
	struct buffer_list low;
	uint16_t type;
	uint16_t protocol;
	union {
		int size;
		uint8_t udp_address[UDP_ADDRESS_SIZE];
	} p;
};

struct socketlib {
	int next_id;
	struct pollfd *event_fd;
	int sendctl_fd;
	int recvctl_fd;
	int check_ctrl;
	fd_set rfds;
	struct event ev[MAX_EVENT];
	struct socket slot[MAX_SOCKET];
	char buffer[MAX_SOCK_INFO];
	int ev_idx;
	int ev_n;
	uint8_t udpbuffer[MAX_UDP_PACKAGE];
	struct socket_object_interface soi;
	socket_alloc alloc;
};

union sockaddr_all {
	struct sockaddr s;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

struct listen_req {
	int id;
	int fd;
	void *ud;
};

struct open_req {
	int id;
	int port;
	void *ud;
	char host[128];
};

struct bind_req {
	int id;
	int fd;
	void *ud;
};

struct start_req {
	int id;
	void *ud;
};

struct close_req {
	int id;
	void *ud;
};

struct send_req {
	int id;
	int priority;
	int size;
	char *data;
};

struct opt_req {
	int id;
	int what;
	int value;
};

struct udp_req {
	int id;
	int fd;
	int family;
	void *ud;
};

struct setudp_req {
	int id;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct sendudp_req {
	struct send_req send;
	uint8_t address[UDP_ADDRESS_SIZE];
};

struct socket_req {
	int req;
	union {
		char dummy[248];
		struct listen_req listen;
		struct open_req open;
		struct bind_req bind;
		struct start_req start;
		struct close_req close;
		struct send_req send;
		struct opt_req opt;
		struct udp_req udp;
		struct setudp_req setudp;
		struct sendudp_req sendudp;
	} u;
};

struct socket_send_object {
	void *data;
	int size;
	void(*free)(void *);
};

static struct socketlib S;

static inline void
socket_nonblocking(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if (-1 == flag) {
		return;
	}
	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

static inline void
socket_keepalive(int fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive, sizeof(keepalive));
}

static int
socket_next_id(void) {
	int i;
	for (i = 0; i < MAX_SOCKET; i++) {
		struct socket *sock;
		int id = atom_inc(&(S.next_id));
		if (id < 0) {
			id = atom_and(&(S.next_id), 0x7fffffff);
		}
		sock = &S.slot[HASH_ID(id)];
		if (sock->type == SOCKET_TYPE_INVALID) {
			if (atom_cas(&(sock->type), SOCKET_TYPE_INVALID, SOCKET_TYPE_RESERVE)) {
				sock->id = id;
				sock->fd = -1;
				return id;
			}
			--i;
		}
	}
	return -1;
}

static struct socket *
socket_new(int fd, int id, int protocol, void *ud, int add) {
	struct socket *sock;
	sock = &S.slot[HASH_ID(id)];
	assert(sock->type == SOCKET_TYPE_RESERVE);
	sock->id = id;
	sock->fd = fd;
	sock->ud = ud;
	sock->p.size = MIN_SOCK_BUFF;
	sock->wb_size = 0;
	sock->protocol = protocol;
	sock->high.head = sock->high.tail = 0;
	sock->low.head = sock->low.tail = 0;
	if (add) {
		if (event_add(S.event_fd, fd, sock)) {
			sock->type = SOCKET_TYPE_INVALID;
			fprintf(stderr, "socketlib event add errno:%d.\n", errno);
			return 0;
		}
	}
	return sock;
}

static inline void
socket_send_req(struct socket_req *req) {
	for (;;) {
		int n = write(S.sendctl_fd, (void *)req, sizeof *req);
		if (n < 0) {
			if (errno != EINTR && errno != EAGAIN) {
				fprintf(stderr, "socketlib pipe send request errno:%d.\n", errno);
				break;
			}
			continue;
		}
		return;
	}
}

static inline int
socket_recv_req(struct socket_req *req) {
	for (;;) {
		int n = read(S.recvctl_fd, (void *)req, sizeof(*req));
		if (n < 0) {
			if (errno != EINTR && errno != EAGAIN) {
				fprintf(stderr, "socketlib pipe recv request errno:%d.\n", errno);
				return -1;
			}
			continue;
		}
		return 0;
	}
}

static int
_socket_bind(const char *host, int port, int protocol, int *family) {
	int fd;
	int status;
	int reuse = 1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = 0;
	char portstr[16];
	if (host == 0 || host[0] == 0) {
		host = "0.0.0.0";
	}
	sprintf(portstr, "%d", port);
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	if (protocol == IPPROTO_TCP) {
		ai_hints.ai_socktype = SOCK_STREAM;
	} else {
		assert(protocol == IPPROTO_UDP);
		ai_hints.ai_socktype = SOCK_DGRAM;
	}
	ai_hints.ai_protocol = protocol;
	status = getaddrinfo(host, portstr, &ai_hints, &ai_list);
	if (status != 0) {
		return -1;
	}
	*family = ai_list->ai_family;
	fd = socket(*family, ai_list->ai_socktype, 0);
	if (fd < 0) {
		goto _failed_fd;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int)) == -1) {
		goto _failed;
	}
	status = bind(fd, (struct sockaddr *)ai_list->ai_addr, ai_list->ai_addrlen);
	if (status != 0) {
		goto _failed;
	}
	freeaddrinfo(ai_list);
	return fd;
_failed:
	close(fd);
_failed_fd:
	freeaddrinfo(ai_list);
	return -1;
}

static inline int
_socket_listen(const char *host, int port) {
	int family = 0;
	int listen_fd = _socket_bind(host, port, IPPROTO_TCP, &family);
	if (listen_fd < 0) {
		return -1;
	}
	if (listen(listen_fd, 512) == -1) {
		close(listen_fd);
		return -1;
	}
	return listen_fd;
}

static inline void
_free_buffer(struct buffer *tmp) {
	if (tmp->send_object)
		S.soi.free(tmp->buff);
	else
		S.alloc(tmp->buff, 0);
	S.alloc(tmp, 0);
}

static inline void
socket_free_buffer_list(struct buffer_list *list) {
	struct buffer *wb = list->head;
	while (wb) {
		struct buffer *tmp = wb;
		wb = wb->next;
		_free_buffer(tmp);
	}
	list->head = list->tail = 0;
}

static void
socket_force_close(struct socket *sock, struct socket_message *ret) {
	ret->id = sock->id;
	ret->ud = sock->ud;
	ret->data = 0;
	ret->size = 0;
	if (sock->type == SOCKET_TYPE_INVALID) {
		return;
	}
	assert(sock->type != SOCKET_TYPE_RESERVE);
	socket_free_buffer_list(&sock->high);
	socket_free_buffer_list(&sock->low);
	if (sock->type != SOCKET_TYPE_PACCEPT && sock->type != SOCKET_TYPE_PLISTEN) {
		event_del(S.event_fd, sock->fd);
	}
	if (sock->type != SOCKET_TYPE_BIND) {
		close(sock->fd);
	}
	sock->type = SOCKET_TYPE_INVALID;
}

static inline void
_soi_free(void *p) {
	S.alloc(p, 0);
}

static inline int
socket_send_object_init(struct socket_send_object *so, void *data, int size) {
	if (size < 0) {
		so->data = S.soi.data(data);
		so->size = S.soi.size(data);
		so->free = S.soi.free;
		return 1;
	} else {
		so->data = data;
		so->size = size;
		so->free = _soi_free;
		return 0;
	}
}

static int
socket_send_buffer_list(struct socket *sock, struct buffer_list *list, struct socket_message *ret) {
	while (list->head) {
		struct buffer *tmp = list->head;
		for (;;) {
			int sz = write(sock->fd, tmp->ptr, tmp->len);
			if (sz < 0) {
				switch (errno) {
				case EINTR:
					continue;
				case EAGAIN:
					return -1;
				}
				fprintf(stderr, "socketlib write to fd %d (fd=%d) errno:%d.\n", sock->id, sock->fd, errno);
				socket_force_close(sock, ret);
				return SOCKET_CLOSE;
			}
			sock->wb_size -= sz;
			if (sz != tmp->len) {
				tmp->ptr += sz;
				tmp->len -= sz;
				return -1;
			}
			break;
		}
		list->head = tmp->next;
		_free_buffer(tmp);
	}
	list->tail = 0;
	return -1;
}

static inline int
socket_buffer_list_complete(struct buffer_list *list) {
	struct buffer *wb = list->head;
	if (wb == 0) {
		return 1;
	}
	return (void *)wb->ptr == wb->buff;
}

static int
socket_send_buffer(struct socket *sock, struct socket_message *ret) {
	assert(socket_buffer_list_complete(&sock->low));
	if (socket_send_buffer_list(sock, &sock->high, ret) == SOCKET_CLOSE) {
		return SOCKET_CLOSE;
	}
	if (sock->high.head == 0) {
		if (sock->low.head != 0) {
			if (socket_send_buffer_list(sock, &sock->low, ret) == SOCKET_CLOSE) {
				return SOCKET_CLOSE;
			}
			if (!socket_buffer_list_complete(&sock->low)) {
				struct buffer_list *high;
				struct buffer_list *low = &sock->low;
				struct buffer *tmp = low->head;
				low->head = tmp->next;
				if (low->head == 0) {
					low->tail = 0;
				}
				high = &sock->high;
				assert(high->head == 0);
				tmp->next = 0;
				high->head = high->tail = tmp;
			}
		} else {
			event_write(S.event_fd, sock->fd, sock, 0);
			if (sock->type == SOCKET_TYPE_HALFCLOSE) {
				socket_force_close(sock, ret);
				return SOCKET_CLOSE;
			}
		}
	}
	return -1;
}

static int
socket_forward_tcp(struct socket *sock, struct socket_message *ret) {
	int n;
	int sz = sock->p.size;
	char *buffer = (char *)S.alloc(0, sz);
	n = (int)read(sock->fd, buffer, sz);
	if (n < 0) {
		S.alloc(buffer, 0);
		switch (errno) {
		case EINTR:
			break;
		case EAGAIN:
			fprintf(stderr, "socketlib forward tcp EAGAIN capture.\n");
			break;
		default:
			socket_force_close(sock, ret);
			ret->data = strerror(errno);
			return SOCKET_ERR;
		}
		return -1;
	}
	if (n == 0) {
		S.alloc(buffer, 0);
		socket_force_close(sock, ret);
		return SOCKET_CLOSE;
	}
	if (sock->type == SOCKET_TYPE_HALFCLOSE) {
		S.alloc(buffer, 0);
		return -1;
	}
	if (n == sz) {
		sock->p.size *= 2;
	} else if (sz > MIN_SOCK_BUFF && n * 2 < sz) {
		sock->p.size /= 2;
	}
	ret->ud = sock->ud;
	ret->id = sock->id;
	ret->size = n;
	ret->data = buffer;
	return SOCKET_DATA;
}

static int
gen_udp_address(int protocol, union sockaddr_all *sa, uint8_t *udp_address) {
	int addrsize = 1;
	udp_address[0] = (uint8_t)protocol;
	if (protocol == PROTOCOL_UDP) {
		memcpy(udp_address + addrsize, &sa->v4.sin_port, sizeof(sa->v4.sin_port));
		addrsize += sizeof(sa->v4.sin_port);
		memcpy(udp_address + addrsize, &sa->v4.sin_addr, sizeof(sa->v4.sin_addr));
		addrsize += sizeof(sa->v4.sin_addr);
	} else {
		memcpy(udp_address + addrsize, &sa->v6.sin6_port, sizeof(sa->v6.sin6_port));
		addrsize += sizeof(sa->v6.sin6_port);
		memcpy(udp_address + addrsize, &sa->v6.sin6_addr, sizeof(sa->v6.sin6_addr));
		addrsize += sizeof(sa->v6.sin6_addr);
	}
	return addrsize;
}

static int
socket_forward_udp(struct socket *sock, struct socket_message *ret) {
	uint8_t *data;
	union sockaddr_all sa;
	socklen_t slen = sizeof(sa);
	int n = recvfrom(sock->fd, S.udpbuffer, MAX_UDP_PACKAGE, 0, &sa.s, &slen);
	if (n < 0) {
		switch (errno) {
		case EINTR:
		case EAGAIN:
			break;
		default:
			socket_force_close(sock, ret);
			ret->data = strerror(errno);
			return SOCKET_ERR;
		}
		return -1;
	}
	if (slen == sizeof(sa.v4)) {
		if (sock->protocol != PROTOCOL_UDP) {
			return -1;
		}
		data = (uint8_t *)S.alloc(0, n + 1 + 2 + 4);
		gen_udp_address(PROTOCOL_UDP, &sa, data + n);
	} else {
		if (sock->protocol != PROTOCOL_UDPv6) {
			return -1;
		}
		data = (uint8_t *)S.alloc(0, n + 1 + 2 + 16);
		gen_udp_address(PROTOCOL_UDPv6, &sa, data + n);
	}
	memcpy(data, S.udpbuffer, n);
	ret->ud = sock->ud;
	ret->id = sock->id;
	ret->size = n;
	ret->data = (char *)data;
	return SOCKET_UDP;
}

static struct buffer *
socket_append_buffer_list(struct buffer_list *list, struct send_req *req, int size, int n) {
	struct buffer *buf = (struct buffer *)S.alloc(0, size);
	struct socket_send_object so;
	buf->send_object = socket_send_object_init(&so, req->data, req->size);
	buf->ptr = (char *)so.data + n;
	buf->len = so.size - n;
	buf->buff = req->data;
	buf->next = 0;
	if (list->head == 0) {
		list->head = list->tail = buf;
	} else {
		assert(list->tail != 0);
		assert(list->tail->next == 0);
		list->tail->next = buf;
		list->tail = buf;
	}
	return buf;
}

static void
socket_append_sendbuffer(struct socket *sock, struct send_req *req, int n) {
	struct buffer *buf;
	if (req->priority == SOCKET_PRIORITY_HIGH) {
		buf = socket_append_buffer_list(&sock->high, req, SIZEOF_TCPBUFFER, n);
	} else if (req->priority == SOCKET_PRIORITY_LOW) {
		buf = socket_append_buffer_list(&sock->low, req, SIZEOF_TCPBUFFER, n);
	} else {
		return;
	}
	sock->wb_size += buf->len;
}

static void
socket_append_udp_sendbuffer(struct socket *sock, struct send_req *req, const uint8_t udp_address[UDP_ADDRESS_SIZE]) {
	struct buffer *buf;
	if (req->priority == SOCKET_PRIORITY_HIGH) {
		buf = socket_append_buffer_list(&sock->high, req, SIZEOF_UDPBUFFER, 0);
	} else if (req->priority == SOCKET_PRIORITY_LOW) {
		buf = socket_append_buffer_list(&sock->low, req, SIZEOF_UDPBUFFER, 0);
	} else {
		return;
	}
	memcpy(buf->udp_address, udp_address, UDP_ADDRESS_SIZE);
	sock->wb_size += buf->len;
}

static int
socket_req_close(struct close_req *req, struct socket_message *msg) {
	struct socket * sock = &S.slot[HASH_ID(req->id)];
	if (sock->type == SOCKET_TYPE_INVALID || sock->id != req->id) {
		msg->id = req->id;
		msg->ud = req->ud;
		msg->data = (char *)"closed";
		msg->size = 0;
		return SOCKET_CLOSE;
	}
	if (sock->high.head != 0 || sock->low.head != 0) {
		int type = socket_send_buffer(sock, msg);
		if (type != -1) return type;
	}
	if (sock->high.head == 0 && sock->low.head == 0) {
		socket_force_close(sock, msg);
		msg->id = req->id;
		msg->ud = req->ud;
		msg->data = (char *)"closed";
		msg->size = 0;
		return SOCKET_CLOSE;
	}
	sock->type = SOCKET_TYPE_HALFCLOSE;
	return -1;
}

static int
socket_req_listen(struct listen_req *req, struct socket_message *msg) {
	struct socket *sock = socket_new(req->fd, req->id, PROTOCOL_TCP, req->ud, 0);
	if (sock == 0) {
		goto _failed;
	}
	sock->type = SOCKET_TYPE_PLISTEN;
	return -1;
_failed:
	close(req->fd);
	msg->ud = req->ud;
	msg->id = req->id;
	msg->data = (char *)"socket limit";
	msg->size = 0;
	S.slot[HASH_ID(req->id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERR;
}

static int
socket_req_open(struct open_req *req, struct socket_message *msg) {
	struct socket *sock;
	int status;
	int fd = -1;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = 0;
	struct addrinfo *ai_ptr = 0;
	char port[16];
	msg->id = req->id;
	msg->ud = req->ud;
	msg->size = 0;
	sprintf(port, "%d", req->port);
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;
	status = getaddrinfo(req->host, port, &ai_hints, &ai_list);
	if (status != 0) {
		msg->data = (char *)gai_strerror(status);
		goto _failed;
	}
	for (ai_ptr = ai_list; ai_ptr != 0; ai_ptr = ai_ptr->ai_next) {
		fd = socket(ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
		if (fd < 0) {
			continue;
		}
		socket_keepalive(fd);
		socket_nonblocking(fd);
		status = connect(fd, ai_ptr->ai_addr, ai_ptr->ai_addrlen);
		if (status != 0 && errno != EINPROGRESS) {
			close(fd);
			fd = -1;
			continue;
		}
		break;
	}
	if (fd < 0) {
		msg->data = strerror(errno);
		goto _failed;
	}
	sock = socket_new(fd, req->id, PROTOCOL_TCP, req->ud, 1);
	if (sock == 0) {
		close(fd);
		msg->data = (char *)"socket limit";
		goto _failed;
	}
	if (status == 0) {
		sock->type = SOCKET_TYPE_OPENED;
		struct sockaddr *addr = ai_ptr->ai_addr;
		void *sin_addr = (ai_ptr->ai_family == AF_INET) ? (void *)&((struct sockaddr_in *)addr)->sin_addr : (void *)&((struct sockaddr_in6 *)addr)->sin6_addr;
		int sin_port = ntohs((ai_ptr->ai_family == AF_INET) ? ((struct sockaddr_in *)addr)->sin_port : ((struct sockaddr_in6 *)addr)->sin6_port);
		char tmp[INET6_ADDRSTRLEN];
		if (inet_ntop(ai_ptr->ai_family, sin_addr, tmp, sizeof(tmp))) {
			snprintf(S.buffer, sizeof(S.buffer), "%s:%d", tmp, sin_port);
			msg->data = S.buffer;
		}
		freeaddrinfo(ai_list);
		return SOCKET_OPEN;
	} else {
		sock->type = SOCKET_TYPE_OPENING;
		event_write(S.event_fd, sock->fd, sock, 1);
	}
	freeaddrinfo(ai_list);
	return -1;
_failed:
	freeaddrinfo(ai_list);
	S.slot[HASH_ID(req->id)].type = SOCKET_TYPE_INVALID;
	return SOCKET_ERR;
}

static int
socket_req_start(struct start_req *req, struct socket_message *msg) {
	struct socket *sock;
	msg->id = req->id;
	msg->ud = req->ud;
	msg->size = 0;
	sock = &S.slot[HASH_ID(req->id)];
	if (sock->type == SOCKET_TYPE_INVALID || sock->id != req->id) {
		msg->data = (char *)"socket invalid id";
		return SOCKET_ERR;
	}
	if (sock->type == SOCKET_TYPE_PACCEPT || sock->type == SOCKET_TYPE_PLISTEN) {
		if (event_add(S.event_fd, sock->fd, sock)) {
			sock->type = SOCKET_TYPE_INVALID;
			msg->data = strerror(errno);
			return SOCKET_ERR;
		}
		sock->ud = req->ud;
		if (sock->type == SOCKET_TYPE_PACCEPT) {
			sock->type = SOCKET_TYPE_OPENED;
			msg->data = (char *)"start";
		} else if (sock->type == SOCKET_TYPE_PLISTEN) {
			sock->type = SOCKET_TYPE_LISTEN;
			msg->data = (char *)"listen";
		}
		return SOCKET_OPEN;
	} else if (sock->type == SOCKET_TYPE_OPENED) {
		sock->ud = req->ud;
		msg->data = (char *)"transfer";
		return SOCKET_OPEN;
	}
	return -1;
}

static int
socket_req_bind(struct bind_req *req, struct socket_message *msg) {
	struct socket *sock;
	msg->id = req->id;
	msg->ud = req->ud;
	msg->size = 0;
	sock = socket_new(req->fd, req->id, PROTOCOL_TCP, req->ud, 1);
	if (sock == 0) {
		msg->data = (char *)"socket limit";
		return SOCKET_ERR;
	}
	socket_nonblocking(req->fd);
	sock->type = SOCKET_TYPE_BIND;
	msg->data = (char *)"binding";
	return SOCKET_OPEN;
}

static socklen_t
udp_socket_address(struct socket *sock, const uint8_t udp_address[UDP_ADDRESS_SIZE], union sockaddr_all *sa) {
	uint16_t port = 0;
	uint8_t type = udp_address[0];
	if (type != sock->protocol) {
		return 0;
	}
	memcpy(&port, udp_address + 1, sizeof(uint16_t));
	switch (sock->protocol) {
	case PROTOCOL_UDP:
		memset(&sa->v4, 0, sizeof(sa->v4));
		sa->s.sa_family = AF_INET;
		sa->v4.sin_port = port;
		memcpy(&sa->v4.sin_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v4.sin_addr));
		return sizeof(sa->v4);
	case PROTOCOL_UDPv6:
		memset(&sa->v6, 0, sizeof(sa->v6));
		sa->s.sa_family = AF_INET6;
		sa->v6.sin6_port = port;
		memcpy(&sa->v6.sin6_addr, udp_address + 1 + sizeof(uint16_t), sizeof(sa->v6.sin6_addr));
		return sizeof(sa->v6);
	}
	return 0;
}

static int
socket_req_send(struct send_req *req, struct socket_message *msg, const uint8_t *udp_address) {
	struct socket * sock = &S.slot[HASH_ID(req->id)];
	struct socket_send_object so;
	socket_send_object_init(&so, req->data, req->size);
	if (sock->type == SOCKET_TYPE_INVALID || sock->id != req->id || sock->type == SOCKET_TYPE_HALFCLOSE
		|| sock->type == SOCKET_TYPE_PACCEPT) {
		so.free(req->data);
		return -1;
	}
	if (sock->type == SOCKET_TYPE_PLISTEN || sock->type == SOCKET_TYPE_LISTEN) {
		fprintf(stderr, "socketlib write to listen fd:%d\n", sock->id);
		so.free(req->data);
		return -1;
	}
	if (sock->high.head == 0 && sock->low.head == 0 && sock->type == SOCKET_TYPE_OPENED) {
		if (sock->protocol == PROTOCOL_TCP) {
			int n = write(sock->fd, so.data, so.size);
			if (n < 0) {
				switch (errno) {
				case EINTR:
				case EAGAIN:
					n = 0;
					break;
				default:
					fprintf(stderr, "socketlib write to %d (fd=%d) errno:%d\n", sock->id, sock->fd, errno);
					socket_force_close(sock, msg);
					so.free(req->data);
					return SOCKET_CLOSE;
				}
			}
			if (n == so.size) {
				so.free(req->data);
				return -1;
			}
			socket_append_sendbuffer(sock, req, n);
		} else {
			int n;
			union sockaddr_all sa;
			socklen_t sa_size;
			if (!udp_address) {
				udp_address = sock->p.udp_address;
			}
			sa_size = udp_socket_address(sock, udp_address, &sa);
			n = sendto(sock->fd, so.data, so.size, 0, &sa.s, sa_size);
			if (n != req->size) {
				socket_append_udp_sendbuffer(sock, req, udp_address);
			} else {
				so.free(req->data);
				return -1;
			}
		}
		event_write(S.event_fd, sock->fd, sock, 1);
	} else {
		if (sock->protocol == PROTOCOL_TCP) {
			socket_append_sendbuffer(sock, req, 0);
		} else {
			if (!udp_address) {
				udp_address = sock->p.udp_address;
			}
			socket_append_udp_sendbuffer(sock, req, udp_address);
		}
	}
	if (sock->wb_size > 1024 * 1024) {
		msg->id = sock->id;
		msg->ud = sock->ud;
		msg->data = 0;
		msg->size = (int)(sock->wb_size / 1024);
		return SOCKET_WARNING;
	}
	return -1;
}

static int
socket_req_opt(struct opt_req *req, struct socket_message *msg) {
	struct socket *sock;
	sock = &S.slot[HASH_ID(req->id)];
	if (sock->type == SOCKET_TYPE_INVALID || sock->id != req->id) {
		return -1;
	}
	setsockopt(sock->fd, IPPROTO_TCP, req->what, (const char *)&req->value, sizeof(req->value));
	return -1;
}

static int
socket_req_setudp(struct setudp_req *req, struct socket_message *msg) {
	int id = req->id;
	int type;
	struct socket *sock;
	sock = &S.slot[HASH_ID(req->id)];
	if (sock->type == SOCKET_TYPE_INVALID || sock->id != id) {
		return -1;
	}
	type = req->address[0];
	if (type != sock->protocol) {
		msg->ud = sock->ud;
		msg->id = id;
		msg->data = (char *)"socket protocol mismatch";
		msg->size = 0;
		return SOCKET_ERR;
	}
	if (type == PROTOCOL_UDP) {
		memcpy(sock->p.udp_address, req->address, 1 + 2 + 4);
	} else {
		memcpy(sock->p.udp_address, req->address, 1 + 2 + 16);
	}
	return -1;
}

static int
socket_req_udp(struct udp_req *req, struct socket_message *msg) {
	int id = req->id;
	int protocol;
	struct socket *sock;
	if (req->family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		protocol = PROTOCOL_UDP;
	}
	sock = socket_new(req->fd, id, protocol, req->ud, 1);
	if (!sock) {
		close(req->fd);
		S.slot[HASH_ID(id)].type = SOCKET_TYPE_INVALID;
		return -1;
	}
	sock->type = SOCKET_TYPE_OPENED;
	memset(sock->p.udp_address, 0, sizeof(sock->p.udp_address));
	return -1;
}

static int
socket_handle_req(struct socket_message *msg) {
	struct socket_req req;
	if (socket_recv_req(&req)) {
		return -1;
	}
	switch (req.req) {
	case SOCKET_REQ_EXIT:
		msg->id = 0;
		msg->ud = 0;
		msg->data = 0;
		msg->size = 0;
		return SOCKET_EXIT;
	case SOCKET_REQ_CLOSE:
		return socket_req_close(&req.u.close, msg);
	case SOCKET_REQ_LISTEN:
		return socket_req_listen(&req.u.listen, msg);
	case SOCKET_REQ_OPEN:
		return socket_req_open(&req.u.open, msg);
	case SOCKET_REQ_START:
		return socket_req_start(&req.u.start, msg);
	case SOCKET_REQ_BIND:
		return socket_req_bind(&req.u.bind, msg);
	case SOCKET_REQ_SEND:
		return socket_req_send(&req.u.send, msg, 0);
	case SOCKET_REQ_OPT:
		return socket_req_opt(&req.u.opt, msg);
	case SOCKET_REQ_SETUDP:
		return socket_req_setudp(&req.u.setudp, msg);
	case SOCKET_REQ_UDP:
		return socket_req_udp(&req.u.udp, msg);
	case SOCKET_REQ_SENDUDP:
		return socket_req_send(&req.u.sendudp.send, msg, req.u.sendudp.address);
	default:
		fprintf(stderr, "socketlib unknown request:%d.\n", req.req);
	}
	return -1;
}

static int
socket_try_open(struct socket *sock, struct socket_message *msg) {
	int error, code;
	socklen_t len = sizeof(error);
	code = getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
	if (code < 0 || error) {
		socket_force_close(sock, msg);
    if (code > 0) {
      msg->data = strerror(error);
    } else {
      msg->data = strerror(errno);
    }
		return SOCKET_ERR;
	} else {
		union sockaddr_all u;
		socklen_t slen = sizeof(u);
		sock->type = SOCKET_TYPE_OPENED;
		if (sock->high.head == 0 && sock->low.head == 0) {
			event_write(S.event_fd, sock->fd, sock, 0);
		}
		if (getpeername(sock->fd, &u.s, &slen) == 0) {
			void *sin_addr = (u.s.sa_family == AF_INET) ? (void *)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
			int sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
			char tmp[INET6_ADDRSTRLEN];
			if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
				snprintf(S.buffer, sizeof(S.buffer), "%s:%d", tmp, sin_port);
				msg->data = S.buffer;
			}
		}
		return SOCKET_OPEN;
	}
}

static int
socket_try_accept(struct socket *sock, struct socket_message *msg) {
	union sockaddr_all u;
	socklen_t len = sizeof(u);
	struct socket *newsock;
	void * sin_addr;
	int sin_port;
	int client_fd, id;
	client_fd = accept(sock->fd, &u.s, &len);
	if (client_fd < 0) {
		if (errno == EMFILE || errno == ENFILE) {
			msg->data = strerror(errno);
			return SOCKET_ERR;
		} else {
			return -1;
		}
	}
	id = socket_next_id();
	if (id < 0) {
		close(client_fd);
		return -1;
	}
	socket_keepalive(client_fd);
	socket_nonblocking(client_fd);
	newsock = socket_new(client_fd, id, PROTOCOL_TCP, sock->ud, 0);
	if (newsock == 0) {
		close(client_fd);
		return -1;
	}
	newsock->type = SOCKET_TYPE_PACCEPT;
	msg->id = sock->id;
	msg->size = newsock->id;
	sin_addr = (u.s.sa_family == AF_INET) ? (void*)&u.v4.sin_addr : (void *)&u.v6.sin6_addr;
	sin_port = ntohs((u.s.sa_family == AF_INET) ? u.v4.sin_port : u.v6.sin6_port);
	char tmp[INET6_ADDRSTRLEN];
	if (inet_ntop(u.s.sa_family, sin_addr, tmp, sizeof(tmp))) {
		snprintf(S.buffer, sizeof(S.buffer), "%s:%d", tmp, sin_port);
		msg->data = S.buffer;
	}
	return SOCKET_ACCEPT;
}

int
socket_init(socket_alloc alloc) {
	int i;
	int fd[2];
	memset(&S, 0, sizeof(S));
	S.event_fd = event_new();
	if (pipe(fd)) {
		fprintf(stderr, "socketlib pipe errno:%d.\n", errno);
		event_free(S.event_fd);
		return -1;
	}
	if (event_add(S.event_fd, fd[0], 0)) {
		close(fd[0]);
		close(fd[1]);
		event_free(S.event_fd);
		fprintf(stderr, "socketlib event add errno:%d.\n", errno);
		return -1;
	}
	S.recvctl_fd = fd[0];
	S.sendctl_fd = fd[1];
	socket_nonblocking(S.recvctl_fd);
	socket_nonblocking(S.sendctl_fd);
	S.check_ctrl = 1;
	FD_ZERO(&S.rfds);
	for (i = 0; i < MAX_SOCKET; i++) {
		struct socket *sock = &S.slot[i];
		sock->type = SOCKET_TYPE_INVALID;
		sock->fd = sock->id = 0;
		sock->high.head = sock->high.tail = 0;
		sock->low.head = sock->low.tail = 0;
	}
	S.ev_idx = S.ev_n = 0;
	S.alloc = alloc;
	return 0;
}

void
socket_unit(void) {
	int i;
	for (i = 0; i < MAX_SOCKET; i++) {
		struct socket *sock = &S.slot[i];
		if (sock->type != SOCKET_TYPE_RESERVE && sock->type != SOCKET_TYPE_INVALID) {
			struct socket_message ret;
			socket_force_close(sock, &ret);
		}
	}
	close(S.sendctl_fd);
	close(S.recvctl_fd);
	event_free(S.event_fd);
}

void
socket_exit(void) {
	struct socket_req req;
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_EXIT;
	socket_send_req(&req);
}

void
socket_start(int id, void *ud) {
	struct socket_req req;
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_START;
	req.u.start.id = id;
	req.u.start.ud = ud;
	socket_send_req(&req);
}

void
socket_close(int id, void *ud) {
	struct socket_req req;
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_CLOSE;
	req.u.close.id = id;
	req.u.close.ud = ud;
	socket_send_req(&req);
}

int
socket_open(const char *host, int port, void *ud) {
	struct socket_req req;
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_OPEN;
	req.u.open.id = socket_next_id();
	req.u.open.ud = ud;
	req.u.open.port = port;
	strcpy(req.u.open.host, host);
	socket_send_req(&req);
	return req.u.open.id;
}

int
socket_listen(const char *host, int port, void *ud) {
	struct socket_req req;
	int fd = _socket_listen(host, port);
	if (fd < 0) return -1;
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_LISTEN;
	req.u.listen.id = socket_next_id();
	req.u.listen.fd = fd;
	req.u.listen.ud = ud;
	socket_send_req(&req);
	return req.u.listen.id;
}

int
socket_bind(int fd, void *ud) {
	struct socket_req req;
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_BIND;
	req.u.bind.id = socket_next_id();
	req.u.bind.fd = fd;
	req.u.bind.ud = ud;
	socket_send_req(&req);
	return req.u.bind.id;
}

static inline void
freebuffer(void *data, int size) {
	struct socket_send_object so;
	socket_send_object_init(&so, data, size);
	so.free(data);
}

long
socket_send(int id, const void *data, int size, int priority) {
	struct socket_req req;
	struct socket * sock = &S.slot[HASH_ID(id)];
	if (sock->id != id || sock->type == SOCKET_TYPE_INVALID) {
		freebuffer((void *)data, size);
		return -1;
	}
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_SEND;
	req.u.send.id = id;
	req.u.send.data = (char *)data;
	req.u.send.size = size;
	req.u.send.priority = priority;
	socket_send_req(&req);
	return sock->wb_size;
}

void
socket_nodelay(int id) {
	struct socket_req req;
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_OPT;
	req.u.opt.id = id;
	req.u.opt.what = TCP_NODELAY;
	req.u.opt.value = 1;
	socket_send_req(&req);
}

static void
clear_closed(int id, int type) {
	if (type == SOCKET_CLOSE || type == SOCKET_ERR) {
		int i;
		for (i = S.ev_idx; i < S.ev_n; i++) {
			struct event *e = &S.ev[i];
			struct socket *s = (struct socket *)e->ud;
			if (s) {
				if (s->type == SOCKET_TYPE_INVALID && s->id == id) {
					e->ud = 0;
					break;
				}
			}
		}
	}
}

static inline int
_socket_has_ctrl(void) {
	struct timeval tv = { 0, 0 };
	FD_SET(S.recvctl_fd, &S.rfds);
	int ret = select(S.recvctl_fd + 1, &S.rfds, 0, 0, &tv);
	return ret > 0;
}

int
socket_poll(struct socket_message *sm) {
	int r = 0;
	for (;;) {
		struct socket *sock;
		struct event *ev;
		if (S.check_ctrl) {
			if (_socket_has_ctrl()) {
				r = socket_handle_req(sm);
				if (-1 != r) {
					clear_closed(sm->id, r);
					goto ret;
				}
				continue;
			} else {
				S.check_ctrl = 0;
			}
		}
		if (S.ev_idx == S.ev_n) {
			S.ev_n = event_wait(S.event_fd, S.ev, MAX_EVENT);
			S.check_ctrl = 1;
			S.ev_idx = 0;
			if (S.ev_n <= 0) {
				S.ev_n = 0;
				if (errno == EINTR) continue;
				fprintf(stderr, "socketlib event wait errno:%d.\n", errno);
				return 0;
			}
		}
		ev = &S.ev[S.ev_idx++];
		sock = (struct socket *)ev->ud;
		if (!sock) {
			continue;
		}
		sm->id = sock->id;
		sm->ud = sock->ud;
		sm->data = 0;
		sm->size = 0;
		switch (sock->type) {
		case SOCKET_TYPE_OPENING:
			r = socket_try_open(sock, sm);
			goto ret;
		case SOCKET_TYPE_LISTEN:
			r = socket_try_accept(sock, sm);
			if (r == -1) break;
			goto ret;
		case SOCKET_TYPE_INVALID:
			break;
		default:
			if (ev->read) {
				if (sock->protocol == PROTOCOL_TCP) {
					r = socket_forward_tcp(sock, sm);
				} else {
					r = socket_forward_udp(sock, sm);
					if (r == SOCKET_UDP) {
						--S.ev_idx;
						goto ret;
					}
				}
				if (ev->write && r != SOCKET_CLOSE && r != SOCKET_ERR) {
					ev->read = 0;
					--S.ev_idx;
				}
				if (r == -1) break;
				goto ret;
			}
			if (ev->write) {
				r = socket_send_buffer(sock, sm);
				if (r == -1) break;
				goto ret;
			}
			break;
		}
	}
ret:
	sm->type = r;
	return r != SOCKET_EXIT;
}

int
socket_udp(const char *host, int port, void *ud) {
	struct socket_req req;
	int fd;
	int family;
	if (port != 0 || host != 0) {
		fd = _socket_bind(host, port, IPPROTO_UDP, &family);
		if (fd < 0) {
			return -1;
		}
	} else {
		family = AF_INET;
		fd = socket(family, SOCK_DGRAM, 0);
		if (fd < 0) {
			return -1;
		}
	}
	socket_nonblocking(fd);
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_UDP;
	req.u.udp.id = socket_next_id();
	req.u.udp.fd = fd;
	req.u.udp.ud = ud;
	req.u.udp.family = family;
	socket_send_req(&req);
	return req.u.udp.id;
}

int
socket_udpopen(int id, const char *host, int port) {
	struct socket_req req;
	struct addrinfo ai_hints;
	struct addrinfo *ai_list = 0;
	char portstr[16];
	int status, protocol;
	sprintf(portstr, "%d", port);
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_DGRAM;
	ai_hints.ai_protocol = IPPROTO_UDP;
	status = getaddrinfo(host, portstr, &ai_hints, &ai_list);
	if (status != 0) {
		return -1;
	}
	if (ai_list->ai_family == AF_INET) {
		protocol = PROTOCOL_UDP;
	} else if (ai_list->ai_family == AF_INET6) {
		protocol = PROTOCOL_UDPv6;
	} else {
		freeaddrinfo(ai_list);
		return -1;
	}
	memset(&req, 0, sizeof req);
	gen_udp_address(protocol, (union sockaddr_all *)ai_list->ai_addr, req.u.setudp.address);
	freeaddrinfo(ai_list);
	req.req = SOCKET_REQ_SETUDP;
	req.u.setudp.id = id;
	socket_send_req(&req);
	return 0;
}

long
socket_udpsend(int id, const char *address, const void *data, int size) {
	const uint8_t *udp_address;
	int addrsize;
	struct socket_req req;
	struct socket * sock = &S.slot[HASH_ID(id)];
	if (sock->id != id || sock->type == SOCKET_TYPE_INVALID) {
		freebuffer((void *)data, size);
		return -1;
	}
	memset(&req, 0, sizeof req);
	req.req = SOCKET_REQ_SENDUDP;
	req.u.sendudp.send.id = id;
	req.u.sendudp.send.data = (char *)data;
	req.u.sendudp.send.size = size;
	udp_address = (const uint8_t *)address;
	switch (udp_address[0]) {
	case PROTOCOL_UDP:
		addrsize = 1 + 2 + 4;
		break;
	case PROTOCOL_UDPv6:
		addrsize = 1 + 2 + 16;
		break;
	default:
		freebuffer((void *)data, size);
		return -1;
	}
	memcpy(req.u.sendudp.address, udp_address, addrsize);
	socket_send_req(&req);
	return sock->wb_size;
}

const char *
socket_udpaddress(struct socket_message *m, int *address_size) {
	uint8_t *udp_address = (uint8_t *)(m->data + m->size);
	int type = udp_address[0];
	switch (type) {
	case PROTOCOL_UDP:
		*address_size = 1 + 2 + 4;
		break;
	case PROTOCOL_UDPv6:
		*address_size = 1 + 2 + 16;
		break;
	default:
		return 0;
	}
	return (const char *)udp_address;
}

void
socket_object(struct socket_object_interface *soi) {
	S.soi = *soi;
}
