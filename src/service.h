#ifndef _service_h_
#define _service_h_

#include <stdint.h>

#define SERVICE_PROTO_RESP 0
#define SERVICE_PROTO_ERROR 1
#define SERVICE_PROTO_SOCKET 2

struct message {
	uint32_t source;
	int proto;
	int session;
	void *data;
	int size;
};

struct module {
	int (*dispatch)(uint32_t, void *ud, const struct message *);
	void *(*create)(uint32_t, const char *param);
	void (*release)(uint32_t, void *ud);
};

void *service_alloc(void *, int);

void service_log(uint32_t handle, const char *fmt, ...);
uint32_t service_create(struct module *module, const char *param);
int service_release(uint32_t handle);
int service_send(uint32_t handle, struct message *m);


int service_session(uint32_t handle);
int service_timeout(uint32_t handle, int ti);

void service_name(const char *name, uint32_t handle);
uint32_t service_query(const char *name);

int service_mqlen(uint32_t handle);

int service_env_init(const char *config);
const char *service_env_get(const char *key);
void service_env_set(const char *key, const char *val);

void service_logon(uint32_t handle);
void service_logoff(uint32_t handle);

void service_start(const char *config);
void service_abort(void);

#endif // _service_h_
