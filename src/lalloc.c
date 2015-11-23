#define _GNU_SOURCE
#include <sys/mman.h>

#include "lalloc.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#define SMALLSIZE 8
#define SMALLLEVEL 32
#define CHUNKSIZE (32 * 1024)
#define HUGESIZE (CHUNKSIZE - 16)
#define BIGSEARCHDEPTH 128

struct chunk {
	struct chunk *next;
};

struct smallblock {
	struct smallblock *next;
};

struct bigblock {
	size_t sz;
	struct bigblock *next;
};

struct hugeblock {
	struct hugeblock *prev;
	struct hugeblock *next;
	size_t sz;
};

struct allocator {
	struct chunk base;
	struct smallblock *small_list[SMALLLEVEL+1];
	struct chunk *chunk_list;
	struct bigblock *big_head;
	struct bigblock *big_tail;
	struct hugeblock huge_list;
	int chunk_used;
};

static inline void *alloc_page(size_t sz) {
	return mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

struct allocator *allocator_new(void) {
	struct allocator *A = alloc_page(CHUNKSIZE);
	memset(A, 0, sizeof *A);
	A->chunk_used = sizeof(struct allocator);
	A->chunk_list = &(A->base);
	A->huge_list.prev = A->huge_list.next = &A->huge_list;
	return A;
}

void allocator_free(struct allocator *A) {
	struct hugeblock *h = A->huge_list.next;
	while (h != &(A->huge_list)) {
		struct hugeblock *nh = h->next;
		munmap(h, h->sz);
		h = nh;
	}
	struct chunk *c = A->chunk_list;
	while (c) {
		struct chunk *nc = c->next;
		munmap(c, CHUNKSIZE);
		c = nc;
	}
}

void allocator_info(struct allocator *A) {
	struct hugeblock *h = A->huge_list.next;
	while (h != &(A->huge_list)) {
		struct hugeblock *nh = h->next;
		printf("huge block %d %d\n", (int)h->sz, ((int)h->sz + 4095) / 4096);
		h = nh;
	}
	int n = 0;
	struct chunk *c = A->chunk_list;
	while (c) {
		++n;
		c = c->next;
	}
	printf("chunk page %d %d\n", CHUNKSIZE, n);
}

static inline void *chunk_new(struct allocator *A, int sz) {
	struct chunk *nc = alloc_page(CHUNKSIZE);
	if (!nc) return 0;
	nc->next = A->chunk_list;
	A->chunk_list = nc;
	A->chunk_used = sizeof(struct chunk) + sz;
	return nc + 1;
}

static void *memory_alloc_small(struct allocator *A, int n) {
	struct smallblock *node = A->small_list[n];
	if (node) {
		A->small_list[n] = node->next;
		return node;
	}
	int sz = (n+1) * SMALLSIZE;
	if (A->chunk_used + sz <= CHUNKSIZE) {
		void *ret = (char *)A->chunk_list + A->chunk_used;
		A->chunk_used += sz;
		return ret;
	}
	int i;
	for (i=n+1; i<=SMALLLEVEL; i++) {
		void *ret = A->small_list[i];
		if (ret) {
			A->small_list[i] = A->small_list[i]->next;
			int idx = i - n - 1;
			struct smallblock *sb = (struct smallblock *)((char *)ret + sz);
			sb->next = A->small_list[idx];
			A->small_list[idx] = sb;
			return ret;
		}
	}
	return chunk_new(A, sz);
}

static inline void memory_free_small(struct allocator *A, struct smallblock *ptr, int n) {
	ptr->next = A->small_list[n];
	A->small_list[n] = ptr;
}

static void *memory_alloc_huge(struct allocator *A, size_t sz) {
	struct hugeblock *h = alloc_page(sizeof(struct hugeblock) + sz);
	if (!h) return 0;
	h->prev = &A->huge_list;
	h->next = A->huge_list.next;
	h->sz = sz;
	A->huge_list.next->prev = h;
	A->huge_list.next = h;
	return h + 1;
}

static void memory_free_huge(struct allocator *A, struct hugeblock *ptr) {
	--ptr;
	ptr->prev->next = ptr->next;
	ptr->next->prev = ptr->prev;
	munmap(ptr, ptr->sz + sizeof(struct hugeblock));
}

static struct bigblock *lookup_biglist(struct allocator *A, int sz) {
	if (!A->big_head) return 0;
	struct bigblock *b = A->big_head;
	if (b == A->big_tail) {
		if (b->sz >= sz) {
			int f = b->sz - sz;
			if (f == 0) {
				A->big_head = A->big_tail = 0;
				return b;
			}
			int idx = (f - 1) / SMALLSIZE;
			void *ptr = (char *)b + sz;
			if (idx < SMALLLEVEL) {
				memory_free_small(A, ptr, idx);
				A->big_head = A->big_tail = 0;
			} else {
				A->big_head = A->big_tail = ptr;
			}
			return b;
		}
		return 0;
	}
	struct bigblock *term = b;
	int n = 0;
	do {
		A->big_head = b->next;
		if (b->sz >= sz) {
			if (b->sz == sz) return b;
			int f = b->sz - sz;
			b->sz = sz;
			int idx = (f - 1) / SMALLSIZE;
			void *ptr = (char *)b + sz;
			if (idx < SMALLLEVEL)
				memory_free_small(A, ptr, idx);
			else {
				struct bigblock *last = ptr;
				last->sz = f;
				if (f > sz) {
					last->next = A->big_head;
					A->big_head = last;
				} else {
					last->next = 0;
					A->big_tail->next = last;
					A->big_tail = last;
				}
			}
			return b;
		}
		b->next = 0;
		A->big_tail->next = b;
		A->big_tail = b;
		b = A->big_head;
		++n;
	} while (b != term && n < BIGSEARCHDEPTH);
	return 0;
}

static void *memory_alloc_big(struct allocator *A, int sz) {
	sz = (sz + sizeof(size_t) + 7) & ~7;
	if (A->chunk_used + sz <= CHUNKSIZE) {
		void *b = (char *)A->chunk_list + A->chunk_used;
		A->chunk_used += sz;
		struct bigblock *bb = b;
		bb->sz = sz;
		return (char *)b + sizeof(size_t);
	}
	struct bigblock *b = lookup_biglist(A, sz);
	if (!b) {
		b = chunk_new(A, sz);
		if (!b) return 0;
		b->sz = sz;
	}
	return (char *)b + sizeof(size_t);
}

static inline void memory_free_big(struct allocator *A, void *ptr) {
	struct bigblock *b = (struct bigblock *)((char *)ptr - sizeof(size_t));
	if (!A->big_head) {
		A->big_head = A->big_tail = b;
		b->next = 0;
	} else {
		b->next = A->big_head;
		A->big_head = b;
	}
}

static inline void *memory_alloc(void *ud, size_t nsize) {
	if (nsize <= SMALLLEVEL * SMALLSIZE) {
		if (nsize == 0) return 0;
		return memory_alloc_small(ud, ((int)(nsize)-1)/SMALLSIZE);
	}
	else if (nsize <= HUGESIZE)
		return memory_alloc_big(ud, (int)nsize);
	else
		return memory_alloc_huge(ud, nsize);
}

static inline void memory_free(void *ud, void *ptr, size_t osize) {
	if (osize <= SMALLLEVEL * SMALLSIZE)
		memory_free_small(ud, ptr, (osize-1)/SMALLSIZE);
	else if (osize <= HUGESIZE)
		memory_free_big(ud, ptr);
	else
		memory_free_huge(ud, ptr);
}

static void *memory_realloc_huge(struct allocator *A, void *ptr, size_t nsize) {
	struct hugeblock *h = (struct hugeblock *)ptr - 1;
	struct hugeblock *nh = mremap(h, h->sz + sizeof(struct hugeblock), nsize + sizeof(struct hugeblock), MREMAP_MAYMOVE);
	if (!nh) return 0;
	nh->sz = nsize;
	if (h == nh) return ptr;
	nh->prev->next = nh;
	nh->next->prev = nh;
	return nh + 1;
}

void *lalloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	if (!ptr) return memory_alloc(ud, nsize);
	else if (nsize == 0) {
		memory_free(ud, ptr, osize);
		return 0;
	}
	else {
		if (osize > HUGESIZE && nsize > HUGESIZE)
			return memory_realloc_huge(ud, ptr, nsize);
		else if (nsize <= osize)
			return ptr;
		else {
			void *tmp = memory_alloc(ud, nsize);
			if (!tmp) return 0;
			memcpy(tmp, ptr, osize);
			memory_free(ud, ptr, osize);
			return tmp;
		}
	}
}
