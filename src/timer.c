#include "timer.h"
#include "lock.h"

#include <assert.h>
#include <time.h>
#include <string.h>

#if defined(__APPLE__)
#include <sys/time.h>
#endif

#define TIMER_NEAR_SHIFT 8
#define TIMER_NEAR (1 << TIMER_NEAR_SHIFT)
#define TIMER_NEAR_MASK (TIMER_NEAR-1)
#define TIMER_LEVEL_SHIFT 6
#define TIMER_LEVEL (1 << TIMER_LEVEL_SHIFT)
#define TIMER_LEVEL_MASK (TIMER_LEVEL-1)

struct timer_node {
	struct timer_node *next;
	uint32_t expire;
};

struct timer_list {
	struct timer_node head;
	struct timer_node *tail;
};

struct timer {
	struct timer_list near[TIMER_NEAR];
	struct timer_list t[4][TIMER_LEVEL];
	timer_dispatch dispatch;
	timer_alloc alloc;
	struct spinlock lock;
	uint32_t time;
	uint32_t current;
	uint32_t start;
	uint64_t cur_pt;
};

static struct timer T;

static uint64_t gettime(void) {
	uint64_t t;
#if !defined(__APPLE__)
#ifdef CLOCK_MONOTONIC_RAW
#define CLOCK_TIMER CLOCK_MONOTONIC_RAW
#else
#define CLOCK_TIMER CLOCK_MONOTONIC
#endif
	struct timespec ti;
	clock_gettime(CLOCK_TIMER, &ti);
	t = (unsigned long long)ti.tv_sec * 100;
	t += ti.tv_nsec / 10000000;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	t = (unsigned long long)tv.tv_sec * 100;
	t += tv.tv_usec / 10000;
#endif
	return t;
}

static void	systime(uint32_t *sec, uint32_t *cs) {
#if !defined(__APPLE__)
	struct timespec ti;
	clock_gettime(CLOCK_REALTIME, &ti);
	*sec = (uint32_t)ti.tv_sec;
	*cs = (uint32_t)(ti.tv_nsec / 10000000);
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	*sec = tv.tv_sec;
	*cs = tv.tv_usec / 10000;
#endif
}

static inline struct timer_node *link_clear(struct timer_list *list) {
	struct timer_node *ret = list->head.next;
	list->head.next = 0;
	list->tail = &(list->head);
	return ret;
}

static inline void link(struct timer_list *list, struct timer_node *node) {
	list->tail->next = node;
	list->tail = node;
	node->next = 0;
}

static void _timer_add_node(struct timer_node *node) {
	uint32_t expire = node->expire;
	uint32_t time = T.time;
	if ((expire|TIMER_NEAR_MASK) == (time|TIMER_NEAR_MASK)) {
		link(&T.near[expire&TIMER_NEAR_MASK], node);
	} else {
		uint32_t i;
		uint32_t mask = TIMER_NEAR << TIMER_LEVEL_SHIFT;
		for (i=0; i<3; i++) {
			if ((expire|(mask-1)) == (time|(mask-1))) {
				break;
			}
			mask <<= TIMER_LEVEL_SHIFT;
		}
		link(&T.t[i][((expire>>(TIMER_NEAR_SHIFT + i*TIMER_LEVEL_SHIFT)) & TIMER_LEVEL_MASK)], node);
	}
}

static void _timer_move_list(int level, int idx) {
	struct timer_node *node = link_clear(&T.t[level][idx]);
	while (node) {
		struct timer_node *tmp = node->next;
		_timer_add_node(node);
		node = tmp;
	}
}

static void _timer_dispatch(struct timer_node *node) {
	do {
		struct timer_node *tmp;
		void *ud = node+1;
		T.dispatch(ud);
		tmp = node;
		node = node->next;
		T.alloc(tmp, 0);
	} while (node);
}

static void _timer_execute(void) {
	int idx = T.time & TIMER_NEAR_MASK;
	while (T.near[idx].head.next) {
		struct timer_node *cur = link_clear(&T.near[idx]);
		spinlock_unlock(&T.lock);
		_timer_dispatch(cur);
		spinlock_lock(&T.lock);
	}
}

static void _timer_shift(void) {
	uint32_t ct = ++T.time;
	if (ct == 0) {
		_timer_move_list(3, 0);
	} else {
		int i = 0, mask = TIMER_NEAR;
		uint32_t time = ct >> TIMER_NEAR_SHIFT;
		while ((ct & (mask-1)) == 0) {
			int idx = time & TIMER_LEVEL_MASK;
			if (idx) {
				_timer_move_list(i, idx);
				break;
			}
			mask <<= TIMER_LEVEL_SHIFT;
			time >>= TIMER_LEVEL_SHIFT;
			i++;
		}
	}
}

static void _timer_update(void) {
	spinlock_lock(&T.lock);
	_timer_execute();
	_timer_shift();
	_timer_execute();
	spinlock_unlock(&T.lock);
}

void timer_init(timer_dispatch dispatch, timer_alloc alloc) {
	int i, j;
	for (i=0; i<TIMER_NEAR; i++) {
		link_clear(&T.near[i]);
	}
	for (i=0; i<4; i++) {
		for (j=0; j<TIMER_LEVEL; j++) {
			link_clear(&T.t[i][j]);
		}
	}
	systime(&T.start, &T.current);
	T.cur_pt = gettime();
	T.dispatch = dispatch;
	T.alloc = alloc;
	spinlock_init(&T.lock);
}

void timer_unit(void) {
	int i, j;
	for (i=0; i<TIMER_NEAR; i++) {
		struct timer_node *node = T.near[i].head.next;
		T.near[i].head.next = 0;
		while (node) {
			struct timer_node *tmp = node;
			node = tmp->next;
			T.alloc(tmp, 0);
		}
	}
	for (i=0; i<4; i++) {
		for (j=0; j<TIMER_LEVEL_MASK; j++) {
			struct timer_node *node = T.t[i][j].head.next;
			T.t[i][j].head.next = 0;
			while (node) {
				struct timer_node *tmp = node;
				node = tmp->next;
				T.alloc(tmp, 0);
			}
		}
	}
	spinlock_unit(&T.lock);
}

void timer_timeout(int time, void *ud, int size) {
	struct timer_node *node = (struct timer_node *)T.alloc(0, sizeof(*node)+size);
	memcpy(node+1, ud, size);
	spinlock_lock(&T.lock);
	node->next = 0;
	node->expire = T.time+time;
	_timer_add_node(node);
	spinlock_unlock(&T.lock);
}

void timer_update(void) {
	uint64_t cp = gettime();
	if (cp < T.cur_pt) {
		T.cur_pt = cp;
	} else if (cp != T.cur_pt) {
		uint32_t i;
		uint32_t oc = T.current;
		uint32_t diff = (uint32_t)(cp - T.cur_pt);
		T.cur_pt = cp;
		T.current += diff;
		if (T.current < oc) {
			T.start += 0xffffffff / 100;
		}
		for (i=0; i<diff; i++) {
			_timer_update();
		}
	}
}

uint32_t timer_starttime(void) {
	return T.start;
}

uint32_t timer_now(void) {
	return T.current;
}
