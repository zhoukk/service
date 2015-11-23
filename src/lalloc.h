#ifndef _lalloc_h_
#define _lalloc_h_

#include <stddef.h>

struct allocator;

struct allocator *allocator_new(void);
void allocator_free(struct allocator *);
void allocator_info(struct allocator *);

void *lalloc(void *ud, void *ptr, size_t osize, size_t nsize);

#endif // _lalloc_h_
