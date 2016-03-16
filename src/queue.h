#ifndef _queue_h_
#define _queue_h_

#include <stdint.h>

struct message;
struct queue;

void worker_queue_init(void);
void worker_queue_unit(void);
void worker_queue_push(struct queue *q);
struct queue *worker_queue_pop(void);

struct queue *queue_create(uint32_t handle);
void queue_release(struct queue *q, void(*dtor)(struct message *, void *), void *ud);
void queue_push(struct queue *q, struct message *m);
struct message *queue_pop(struct queue *q, struct message *m);
int queue_length(struct queue *q);
int queue_overload(struct queue *q);
uint32_t queue_handle(struct queue *q);
void queue_try_release(struct queue *q);

#endif // _queue_h_
