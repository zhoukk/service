#include "service.h"
#include "index.h"
#include "queue.h"
#include "lock.h"
#include "env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

struct service {
	uint32_t handle;
	struct module module;
	void *ud;
	int session;
	struct queue *queue;
	FILE *logfile;
};

struct service_global {
	int total;
	struct worker_queue *wq;
	struct index *index;
	struct env *env;
	struct env *names;
	uint32_t log;
};

static struct service_global g;

void *service_alloc(void *p, int size) {
	if (0 == size) {
		if (p) free(p);
		return 0;
	}
	p = malloc(size);
	memset(p, 0, size);
	return p;
}

void service_log(uint32_t handle, const char *fmt, ...) {
	if (g.log == 0) {
		fprintf(stderr, "[%u] ", handle);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	} else {
		struct message m;
		int size;
		va_list ap;
		va_start(ap, fmt);
		size = vsnprintf(0, 0, fmt, ap);
		va_end(ap);
		m.data = service_alloc(0, size+1);
		va_start(ap, fmt);
		vsnprintf((char *)m.data, size+1, fmt, ap);
		va_end(ap);
		m.size = size+1;
		m.session = 0;
		m.proto = 0;
		m.source = handle;
		if (-1 == service_send(g.log, &m))
			service_alloc(m.data, 0);
	}
}

static void initialize(const char *config) {
	g.env = env_create(config);
	if (!g.env) {
		fprintf(stderr, "usage: ./service config\n");
		exit(0);
	}
	g.log = 0;
	g.total = 0;
	g.index = index_new();
	g.names = env_create(0);
}

static void finalize(void) {
	index_free(g.index);
	env_release(g.names);
	env_release(g.env);
}

static inline void service_total_inc(void) {
	atom_inc(&g.total);
}

static inline void service_total_dec(void) {
	atom_dec(&g.total);
}

static inline int service_total(void) {
	return g.total;
}

void service_name(const char *name, uint32_t handle) {
	env_setint(g.names, name, handle);
}

uint32_t service_query(const char *name) {
	return (uint32_t)env_getint(g.names, name);
}

const char *service_env_get(const char *key) {
	return env_getstr(g.env, key);
}

void service_env_set(const char *key, const char *val) {
	env_setstr(g.env, key, val);
}

void service_abort(void) {
	int n = service_total();
	uint32_t list[n];
	n = index_list(g.index, n, list);
	int i;
	for (i=0; i<n; i++)
		service_release(list[i]);
}

static inline uint32_t service_regist(struct service *s) {
	return index_regist(g.index, s);
}

static inline struct service *service_grab(uint32_t handle) {
	return (struct service *)index_grab(g.index, handle);
}

static void message_queue_message_dtor(struct message *m, void *ud) {
	service_alloc(m->data, 0);
	struct message em;
	em.source = (uint32_t)(uintptr_t)ud;
	em.session = 0;
	em.data = 0;
	em.size = 0;
	service_send(m->source, &em);
}

uint32_t service_create(struct module *module, const char *param) {
	struct service *s = service_alloc(0, sizeof *s);
	s->module = *module;
	s->session = 0;
	s->logfile = 0;
	s->handle = service_regist(s);
	s->queue = message_queue_create(s->handle);
	service_total_inc();

	s = service_grab(s->handle);
	s->ud = module->create(s->handle, param);
	if (!s->ud) {
		service_log(s->handle, "FAILED %s\n", param);
		uint32_t handle = s->handle;
		while (!(s = (struct service *)index_release(g.index, handle))) {}
		message_queue_try_release(s->queue);
		message_queue_release(s->queue, message_queue_message_dtor, (void *)(uintptr_t)handle);
		service_alloc(s, 0);
		service_total_dec();
		return 0;
	}
	service_log(s->handle, "CREATE %s\n", param);
	worker_queue_push(s->queue);
	if (service_release(s->handle))
		return 0;
	return s->handle;
}

int service_release(uint32_t handle) {
	struct service *s = (struct service *)index_release(g.index, handle);
	if (!s) return 0;
	s->module.release(s->handle, s->ud);
	message_queue_try_release(s->queue);
	if (s->logfile)
		fclose(s->logfile);
	service_alloc(s, 0);
	service_total_dec();
	service_log(handle, "RELEASE\n");
	return 1;
}

int service_send(uint32_t handle, struct message *m) {
	struct service *s = service_grab(handle);
	if (!s) return -1;
	message_queue_push(s->queue, m);
	service_release(handle);
	return m->session;
}

int service_session(uint32_t handle) {
	struct service *s = service_grab(handle);
	if (!s) return -1;
	int session = ++s->session;
	service_release(handle);
	return session;
}

int service_mqlen(uint32_t handle) {
	struct service *s = service_grab(handle);
	if (!s) return 0;
	int mqlen = message_queue_length(s->queue);
	service_release(handle);
	return mqlen;
}

void log_output(FILE *f, struct message *m);

struct monitor;
void monitor_trigger(struct monitor *monitor, uint32_t source, uint32_t handle);

struct queue *service_dispatch(struct monitor *monitor, struct worker_queue *wq, struct queue *q) {
	if (!q) {
		q = worker_queue_pop(wq);
		if (!q) return 0;
	}

	uint32_t handle = message_queue_handle(q);
	struct service *s = service_grab(handle);
	if (!s) {
		message_queue_release(q, message_queue_message_dtor, (void *)(uintptr_t)handle);
		return worker_queue_pop(wq);
	}
	struct message m;
	if (!message_queue_pop(q, &m)) {
		service_release(handle);
		return worker_queue_pop(wq);
	}
	int overload = message_queue_overload(q);
	if (overload)
		service_log(handle, "service may overload, message queue length = %d\n", overload);
	monitor_trigger(monitor, m.source, handle);
	if (s->logfile)
		log_output(s->logfile, &m);
	s->module.dispatch(s->handle, s->ud, &m);
	service_alloc(m.data, 0);
	monitor_trigger(monitor, 0, 0);
	struct queue *next = worker_queue_pop(wq);
	if (next) {
		worker_queue_push(wq, q);
		q = next;
	}
	service_release(handle);
	return q;
}

#include <unistd.h>
#include <pthread.h>

#include "timer.h"
#include "socket.h"
#include "dump.h"

static FILE *log_open(uint32_t handle) {
	char tmp[128];
	FILE *f;
	sprintf(tmp, "%u.log", handle);
	f = fopen(tmp, "ab");
	if (f) {
		uint32_t starttime = timer_starttime();
		uint32_t curtime = timer_now();
		time_t ti = starttime + curtime/100;
		service_log(handle, "open log file %s\n", tmp);
		fprintf(f, "open time:%u %s", curtime, ctime(&ti));
		fflush(f);
	} else {
		service_log(handle, "open log file %s failed\n", tmp);
	}
	return f;
}

static void log_close(uint32_t handle, FILE *f) {
	service_log(handle, "close log file %u\n", handle);
	fprintf(f, "close time:%u\n", timer_now());
	fclose(f);
}

static void print(void *ud, const char *line) {
	fprintf((FILE *)ud, "%s", line);
}

static void log_blob(FILE *f, void *data, int size) {
	dump((const unsigned char *)data, size, print, (void *)f);
}

static void log_socket(FILE *f, void *data) {
	struct socket_message *m = (struct socket_message *)data;
	if (m->type == SOCKET_DATA) {
		fprintf(f, "[socket] %d %d %d\n", m->id, m->type, m->size);
		log_blob(f, m->data, m->size);
		fprintf(f, "\n");
		fflush(f);
	}
}

void log_output(FILE *f, struct message *m) {
	if (m->proto == SERVICE_PROTO_SOCKET) {
		log_socket(f, m->data);
	} else {
		uint32_t ti = timer_now();
		fprintf(f, "[%u] %d %d %u\n", m->source, m->proto, m->session, ti);
		log_blob(f, m->data, m->size);
		fprintf(f, "\n");
		fflush(f);
	}
}

void service_logon(uint32_t handle) {
	struct service *s = service_grab(handle);
	if (!s) return;
	if (!s->logfile) {
		FILE *f = log_open(handle);
		if (f) {
			if (!atom_cas(&s->logfile, 0, f))
				fclose(f);
		}
	}
	service_release(handle);
}

void service_logoff(uint32_t handle) {
	struct service *s = service_grab(handle);
	if (!s) return;
	FILE *f = s->logfile;
	if (f) {
		if (atom_cas(&s->logfile, f, 0))
			log_close(handle, f);
	}
	service_release(handle);
}

struct timer_event {
	int session;
	uint32_t handle;
};

int service_timeout(uint32_t handle, int ti) {
	int session = service_session(handle);
	if (ti == 0) {
		struct message m;
		m.source = handle;
		m.session = session;
		m.data = 0;
		m.size = 0;
		m.proto = SERVICE_PROTO_RESP;
		session = service_send(handle, &m);
	} else {
		struct timer_event evt;
		evt.session = session;
		evt.handle = handle;
		timer_timeout(ti, &evt, sizeof(evt));
	}
	return session;
}

static void service_timer_dispatch(void *p) {
	struct timer_event *evt = (struct timer_event *)p;
	struct message m;
	m.source = evt->handle;
	m.session = evt->session;
	m.data = 0;
	m.size = 0;
	m.proto = SERVICE_PROTO_RESP;
	service_send(evt->handle, &m);
}

static void service_socket_poll(void) {
	struct socket_message sm;
	if (!socket_poll(&sm))
		return;
	struct message m;
	if (sm.type == SOCKET_EXIT) {
		return;
	}
	int size = sizeof sm;
	m.source = 0;
	m.session = 0;
	m.data = service_alloc(0, size);
	m.size = size;
	m.proto = SERVICE_PROTO_SOCKET;
	memcpy(m.data, &sm, size);
	uint32_t handle = (uint32_t)(uintptr_t)sm.ud;
	if (-1 == service_send(handle, &m))
		service_alloc(m.data, 0);
}

extern struct module lua_mod;


struct watcher {
	int thread;
	int sleep;
	int quit;
	struct worker_param *wp;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

struct monitor {
	int version;
	int check_version;
	uint32_t source;
	uint32_t handle;
};

struct worker_param {
	int thread;
	struct watcher *watcher;
	struct monitor monitor;
};

static inline void watcher_init(struct watcher *watcher, int thread, struct worker_param *wp) {
	watcher->thread = thread;
	watcher->sleep = 0;
	watcher->quit = 0;
	watcher->wp = wp;
	pthread_mutex_init(&watcher->mutex, 0);
	pthread_cond_init(&watcher->cond, 0);
}

static inline void watcher_unit(struct watcher *watcher) {
	pthread_mutex_destroy(&watcher->mutex);
	pthread_cond_destroy(&watcher->cond);
}

static inline void monitor_init(struct monitor *monitor) {
	monitor->source = monitor->handle = 0;
	monitor->version = monitor->check_version = 0;
}

static inline void monitor_check(struct monitor *monitor) {
	if (monitor->version == monitor->check_version) {
		if (monitor->handle) {
			service_log(monitor->handle, "message from [%u] to [%u] maybe in endless loop (version=%d)\n",
				monitor->source, monitor->handle, monitor->version);
		}
	} else {
		monitor->check_version = monitor->version;
	}
}

void monitor_trigger(struct monitor *monitor, uint32_t source, uint32_t handle) {
	monitor->source = source;
	monitor->handle = handle;
	atom_inc(&monitor->version);
}

static void *worker(void *p) {
	struct worker_param *wp = (struct worker_param *)p;
	struct watcher *watcher = wp->watcher;
	int thread = wp->thread;
	struct worker_queue *wq = &g.wq[thread];
	struct queue *q = 0;
	while (!watcher->quit) {
		q = service_dispatch(&wp->monitor, wq, q);
		if (!q) {
			pthread_mutex_lock(&watcher->mutex);
			++watcher->sleep;
			if (!watcher->quit)
				pthread_cond_wait(&watcher->cond, &watcher->mutex);
			--watcher->sleep;
			pthread_mutex_unlock(&watcher->mutex);
		}
	}
	return 0;
}

static void *timer(void *p) {
	struct watcher *watcher = (struct watcher *)p;
	while (1) {
		timer_update();
		if (service_total() == 0)
			break;
		if (watcher->sleep >= 1)
			pthread_cond_signal(&watcher->cond);
		usleep(2500);
	}
	socket_exit();
	pthread_mutex_lock(&watcher->mutex);
	watcher->quit = 1;
	pthread_cond_broadcast(&watcher->cond);
	pthread_mutex_unlock(&watcher->mutex);
	return 0;
}

static void *socket(void *p) {
	struct watcher *watcher = (struct watcher *)p;
	while (service_socket_poll()) {
		if (watcher->sleep >= watcher->thread)
			pthread_cond_signal(&watcher->cond);
	}
	return 0;
}

static void *monitor(void *p) {
	struct watcher *watcher = (struct watcher *)p;
	while (1) {
		if (service_total() == 0)
			break;
		int i;
		for (i=0; i<watcher->thread; i++)
			monitor_check(&watcher->wp[i].monitor);
		for (i=0; i<5; i++) {
			if (service_total() == 0)
				break;
			sleep(1);
		}
	}
	return 0;
}


static void start(int thread) {
	struct worker_param wp[thread];
	struct watcher watcher;
	watcher_init(&watcher, thread, wp);

	struct worker_queue wq[thread];
	g.wq = wq;

	pthread_t pid[thread+3];
	int i;
	for (i=0; i<thread; i++) {
		wp[i].watcher = &watcher;
		wp[i].thread = i;
		worker_queue_init(&wq[i]);
		monitor_init(&wp[i].monitor);
		pthread_create(&pid[i], 0, worker, &wp[i]);
	}

	pthread_create(&pid[i++], 0, timer, &watcher);
	pthread_create(&pid[i++], 0, socket, &watcher);
	pthread_create(&pid[i++], 0, monitor, &watcher);

	for (i=0; i<thread+3; i++)
		pthread_join(pid[i], 0);
	watcher_unit(&watcher);
}


static void *log_create(uint32_t handle, const char *param) {
	FILE *f;
	if (param)
		f = fopen(param, "w");
	else
		f = stderr;
	return f;
}

static void log_release(uint32_t handle, void *ud) {
	FILE *f = (FILE *)ud;
	if (f != stderr)
		fclose(f);
}

static int log_dispatch(uint32_t handle, void *ud, const struct message *m) {
	FILE *f = (FILE *)ud;
	fprintf(f, "[%u] ", m->source);
	fwrite(m->data, m->size, 1, f);
	fflush(f);
	return 0;
}

void service_start(const char *config) {
	initialize(config);
	timer_init(service_timer_dispatch, service_alloc);
	socket_init(service_alloc);

	struct module log_mod = {
		log_dispatch,
		log_create,
		log_release,
	};

	g.log = service_create(&log_mod, service_env_get("log"));
	service_create(&lua_mod, service_env_get("main"));

	int thread = atoi(service_env_get("thread"));

	start(thread);

	socket_unit();
	timer_unit();
	finalize();
}
