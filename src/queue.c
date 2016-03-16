#include "queue.h"
#include "service.h"
#include "lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define QUEUE_CAP 16
#define QUEUE_OVERLOAD 1024

struct queue {
	uint32_t handle;
	int cap;
	int head;
	int tail;
	int global;
	int release;
	int overload;
	int overload_threshold;
	struct spinlock lock;
	struct queue *next;
	struct message *slot;
};

struct worker_queue {
	struct queue *head;
	struct queue *tail;
	struct spinlock lock;
};

static struct worker_queue WQ;

void
worker_queue_init(void) {
	WQ.head = WQ.tail = 0;
	spinlock_init(&WQ.lock);
}

void
worker_queue_unit(void) {
	WQ.head = WQ.tail = 0;
	spinlock_unit(&WQ.lock);
}

void
worker_queue_push(struct queue *q) {
	spinlock_lock(&WQ.lock);
	if (WQ.tail) {
		WQ.tail->next = q;
		WQ.tail = q;
	} else {
		WQ.head = WQ.tail = q;
	}
	spinlock_unlock(&WQ.lock);
}

struct queue *
worker_queue_pop(void) {
	struct queue *q;
	spinlock_lock(&WQ.lock);
	q = WQ.head;
	if (q) {
		WQ.head = q->next;
		if (!WQ.head) WQ.tail = 0;
		q->next = 0;
	}
	spinlock_unlock(&WQ.lock);
	return q;
}

struct queue *
queue_create(uint32_t handle) {
	struct queue *q = (struct queue *)service_alloc(0, sizeof *q);
	q->handle = handle;
	q->cap = QUEUE_CAP;
	q->head = q->tail = 0;
	q->next = 0;
	q->overload = 0;
	q->overload_threshold = QUEUE_OVERLOAD;
	q->release = 0;
	q->global = 1;
	q->slot = (struct message *)service_alloc(0, q->cap * sizeof(struct message));
	spinlock_init(&q->lock);
	return q;
}

void
queue_release(struct queue *q, void(*dtor)(struct message *, void *), void *ud) {
	spinlock_lock(&q->lock);
	if (q->release == 0) {
		worker_queue_push(q);
		spinlock_unlock(&q->lock);
		return;
	}
	spinlock_unlock(&q->lock);
	if (dtor) {
		struct message m;
		while (queue_pop(q, &m)) {
			dtor(&m, ud);
		}
	}
	spinlock_unit(&q->lock);
	service_alloc(q, 0);
}

void
queue_try_release(struct queue *q) {
	spinlock_lock(&q->lock);
	q->release = 1;
	if (q->global == 0)
		worker_queue_push(q);
	spinlock_unlock(&q->lock);
}

void
queue_push(struct queue *q, struct message *m) {
	spinlock_lock(&q->lock);
	q->slot[q->tail++] = *m;
	if (q->tail >= q->cap) q->tail = 0;
	if (q->tail == q->head) {
		struct message *slot = (struct message *)service_alloc(0, sizeof(struct message) * q->cap * 2);
		assert(slot);
		int i;
		int head = q->head;
		for (i = 0; i < q->cap; i++) {
			slot[i] = q->slot[head&(q->cap-1)];
			++head;
		}
		service_alloc(q->slot, 0);
		q->slot = slot;
		q->head = 0;
		q->tail = q->cap;
		q->cap *= 2;
	}
	if (q->global == 0) {
		q->global = 1;
		worker_queue_push(q);
	}
	spinlock_unlock(&q->lock);
}

struct message *
queue_pop(struct queue *q, struct message *m) {
	spinlock_lock(&q->lock);
	if (q->head != q->tail) {
		*m = q->slot[q->head++];
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;
		if (head >= cap) q->head = head = 0;
		int len = tail - head;
		if (len < 0) len += cap;
		while (len > q->overload_threshold) {
			q->overload = len;
			q->overload_threshold *= 2;
		}
	} else {
		m = 0;
		q->global = 0;
		q->overload_threshold = QUEUE_OVERLOAD;
	}
	spinlock_unlock(&q->lock);
	return m;
}

int
queue_overload(struct queue *q) {
	if (q->overload) {
		int overload = q->overload;
		q->overload = 0;
		return overload;
	}
	return 0;
}

int
queue_length(struct queue *q) {
	int head, tail, cap;
	spinlock_lock(&q->lock);
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	spinlock_unlock(&q->lock);
	if (head <= tail) return tail - head;
	return tail + cap - head;
}

uint32_t
queue_handle(struct queue *q) {
	return q->handle;
}
