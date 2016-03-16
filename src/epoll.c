#include "event.h"

#include <unistd.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>

struct pollfd {
	int event_fd;
};

struct pollfd *
event_new(void) {
	struct pollfd *pfd = (struct pollfd *)malloc(sizeof *pfd);
	pfd->event_fd = epoll_create1(0);
	if (pfd->event_fd == -1) {
		free(pfd);
		return NULL;
	}
	return pfd;
}

void
event_free(struct pollfd *pfd) {
	close(pfd->event_fd);
	free(pfd);
}

int
event_add(struct pollfd *pfd, int fd, void *ud) {
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = ud;
	if (epoll_ctl(pfd->event_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		return 1;
	}
	return 0;
}

void
event_del(struct pollfd *pfd, int fd) {
	epoll_ctl(pfd->event_fd, EPOLL_CTL_DEL, fd , 0);
}

void
event_write(struct pollfd *pfd, int fd, void *ud, int enable) {
	struct epoll_event ev;
	ev.events = EPOLLIN | (enable ? EPOLLOUT : 0);
	ev.data.ptr = ud;
	epoll_ctl(pfd->event_fd, EPOLL_CTL_MOD, fd, &ev);
}

int
event_wait(struct pollfd *pfd, struct event *e, int maxev) {
	struct epoll_event ev[maxev];
	int i, n;
	n = epoll_wait(pfd->event_fd, ev, maxev, -1);
	for (i = 0; i < n; i++) {
		unsigned flag = ev[i].events;
		e[i].ud = ev[i].data.ptr;
		e[i].write = (flag & EPOLLOUT) != 0;
		e[i].read = (flag & EPOLLIN) != 0;
	}
	return n;
}
