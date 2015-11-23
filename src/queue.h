#ifndef _queue_h_
#define _queue_h_

#include <stdint.h>

struct message;
struct message_queue;

struct worker_queue {
	struct message_queue *head;
	struct message_queue *tail;
	struct spinlock lock;
};

void worker_queue_init(struct worker_queue *wq);
void worker_queue_unit(struct worker_queue *wq);
void worker_queue_push(struct worker_queue *wq, struct message_queue *mq);
struct message_queue *worker_queue_pop(struct worker_queue *wq);

struct message_queue *message_queue_create(uint32_t handle);
void message_queue_release(struct message_queue *mq, void(*dtor)(struct message *, void *), void *ud);
void message_queue_push(struct message_queue *mq, struct message *m);
struct message *message_queue_pop(struct message_queue *mq, struct message *m);
int message_queue_length(struct message_queue *mq);
int message_queue_overload(struct message_queue *mq);
uint32_t message_queue_handle(struct message_queue *mq);
void message_queue_try_release(struct message_queue *mq);

#endif // _queue_h_
