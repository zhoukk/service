#ifndef _event_h_
#define _event_h_

struct event {
	int read;
	int write;
	void *ud;
};

struct pollfd;
struct pollfd *event_new(void);
void event_free(struct pollfd *pfd);
int event_add(struct pollfd *pfd, int fd, void *ud);
void event_del(struct pollfd *pfd, int fd);
void event_write(struct pollfd *pfd, int fd, void *ud, int enable);
int event_wait(struct pollfd *pfd, struct event *e, int maxev);

#endif // _event_h_
