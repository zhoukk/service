#include "queue.h"
#include "service.h"
#include "lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define QUEUE_CAP 16
#define QUEUE_OVERLOAD 1024

struct message_queue {
	uint32_t handle;
	int cap;
	int head;
	int tail;
	int global;
	int release;
	int overload;
	int overload_threshold;
	struct spinlock lock;
	struct worker_queue *wq;
	struct message_queue *next;
	struct message *slot;
};

void worker_queue_init(struct worker_queue *wq) {
	wq->head = wq->tail = 0;
	spinlock_init(&wq->lock);
}

void worker_queue_unit(struct worker_queue *wq) {
	wq->head = wq->tail = 0;
	spinlock_unit(&wq->lock);
}

void worker_queue_push(struct worker_queue *wq, struct message_queue *mq) {
	spinlock_lock(&wq->lock);
	if (wq->tail) {
		wq->tail->next = mq;
		wq->tail = mq;
	} else {
		wq->head = wq->tail = mq;
	}
	spinlock_unlock(&wq->lock);
}

struct message_queue *worker_queue_pop(struct worker_queue *wq) {
	struct message_queue *mq;
	spinlock_lock(&wq->lock);
	mq = wq->head;
	if (mq) {
		wq->head = mq->next;
		if (!wq->head) wq->tail = 0;
		mq->next = 0;
	}
	spinlock_unlock(&wq->lock);
	return mq;
}

struct message_queue *message_queue_create(uint32_t handle) {
	struct message_queue *mq = (struct message_queue *)service_alloc(0, sizeof *mq);
	mq->handle = handle;
	mq->cap = QUEUE_CAP;
	mq->head = mq->tail = 0;
	mq->next = 0;
	mq->overload = 0;
	mq->overload_threshold = QUEUE_OVERLOAD;
	mq->release = 0;
	mq->global = 1;
	mq->wq = 0;
	mq->slot = (struct message *)service_alloc(0, mq->cap * sizeof(struct message));
	spinlock_init(&mq->lock);
	return mq;
}

void message_queue_release(struct message_queue *mq, void(*dtor)(struct message *, void *), void *ud) {
	spinlock_lock(&mq->lock);
	if (mq->release == 0) {
		worker_queue_push(mq->wq, mq);
		spinlock_unlock(&mq->lock);
		return;
	}
	spinlock_unlock(&mq->lock);
	if (dtor) {
		struct message m;
		while (message_queue_pop(mq, &m)) {
			dtor(&m, ud);
		}
	}
	spinlock_unit(&mq->lock);
	service_alloc(mq, 0);
}

void queue_try_release(struct message_queue *mq) {
	spinlock_lock(&mq->lock);
	mq->release = 1;
	if (mq->global == 0)
		worker_queue_push(mq->wq, mq);
	spinlock_unlock(&mq->lock);
}

void message_queue_push(struct message_queue *mq, struct message *m) {
	spinlock_lock(&mq->lock);
	mq->slot[mq->tail++] = *m;
	if (mq->tail >= mq->cap) mq->tail = 0;
	if (mq->tail == mq->head) {
		struct message *slot = (struct message *)service_alloc(0, sizeof(struct message) * q->cap * 2);
		assert(slot);
		int i;
		int head = mq->head;
		for (i = 0; i < mq->cap; i++) {
			slot[i] = mq->slot[head&(mq->cap-1)];
			++head;
		}
		service_alloc(mq->slot, 0);
		mq->slot = slot;
		mq->head = 0;
		mq->tail = mq->cap;
		mq->cap *= 2;
	}
	if (mq->global == 0) {
		mq->global = 1;
		worker_queue_push(mq->wq, mq);
	}
	spinlock_unlock(&mq->lock);
}

struct message *message_queue_pop(struct message_queue *mq, struct message *m) {
	spinlock_lock(&mq->lock);
	if (mq->head != mq->tail) {
		*m = mq->slot[mq->head++];
		int head = mq->head;
		int tail = mq->tail;
		int cap = mq->cap;
		if (head >= cap) mq->head = head = 0;
		int len = tail - head;
		if (len < 0) len += cap;
		while (len > mq->overload_threshold) {
			mq->overload = len;
			mq->overload_threshold *= 2;
		}
	} else {
		m = 0;
		mq->global = 0;
		mq->overload_threshold = QUEUE_OVERLOAD;
	}
	spinlock_unlock(&mq->lock);
	return m;
}

int message_queue_overload(struct message_queue *mq) {
	if (mq->overload) {
		int overload = mq->overload;
		mq->overload = 0;
		return overload;
	}
	return 0;
}

int message_queue_length(struct message_queue *mq) {
	int head, tail, cap;
	spinlock_lock(&mq->lock);
	head = mq->head;
	tail = mq->tail;
	cap = mq->cap;
	spinlock_unlock(&mq->lock);
	if (head <= tail) return tail - head;
	return tail + cap - head;
}

uint32_t message_queue_handle(struct message_queue *mq) {
	return mq->handle;
}