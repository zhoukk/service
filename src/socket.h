#ifndef _socket_h_
#define _socket_h_

#define SOCKET_EXIT 0
#define SOCKET_CLOSE 1
#define SOCKET_OPEN 2
#define SOCKET_DATA 3
#define SOCKET_ACCEPT 4
#define SOCKET_ERR 5
#define SOCKET_UDP 6
#define SOCKET_WARNING 7

#define SOCKET_PRIORITY_HIGH 0
#define SOCKET_PRIORITY_LOW 1


struct socket_message {
	int type;
	int id;
	void *ud;
	char *data;
	int size;
};

typedef void *(*socket_alloc)(void *, int size);

int socket_init(socket_alloc);
void socket_unit(void);

void socket_exit(void);
void socket_start(int id, void *ud);
void socket_close(int id, void *ud);
void socket_nodelay(int id);
int socket_open(const char *host, int port, void *ud);
int socket_listen(const char *host, int port, void *ud);
int socket_bind(int fd, void *ud);
long socket_send(int id, const void *data, int size, int priority);
int socket_poll(struct socket_message *sm);

int socket_udp(const char *host, int port, void *ud);
int socket_udpopen(int id, const char *host, int port);
long socket_udpsend(int id, const char *addr, const void *data, int size);
const char *socket_udpaddress(struct socket_message *m, int *address_size);

struct socket_object_interface {
	void *(*data)(void *);
	int(*size)(void *);
	void(*free)(void *);
};
void socket_object(struct socket_object_interface *soi);


#endif // _socket_h_
