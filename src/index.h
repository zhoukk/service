#ifndef _index_h_
#define _index_h_

#include <stdint.h>

typedef uint32_t id_t;

struct index;

struct index *index_new(void);
void index_free(struct index *);

id_t index_regist(struct index *, void *ud);
void *index_grab(struct index *, id_t id);
void *index_release(struct index *, id_t id);
int index_list(struct index *, int n, id_t *list);

#endif // _index_h_
