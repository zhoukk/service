#ifndef _timer_h_
#define _timer_h_

#include <stdint.h>

typedef void (*timer_dispatch)(void *);
typedef void *(*timer_alloc)(void *, int);

void timer_init(timer_dispatch, timer_alloc);
void timer_unit(void);
void timer_timeout(int time, void *ud, int size);
void timer_update(void);
uint32_t timer_starttime(void);
uint32_t timer_now(void);

#endif // _timer_h_
