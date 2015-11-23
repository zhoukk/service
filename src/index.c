#include "index.h"
#include "lock.h"

#include <stdio.h>
#include <stdlib.h>

#define INDEX_SLOT 16
#define HASH(id) (id&(idx->cap-1))

struct slot {
	id_t id;
	int ref;
	void *ud;
};

struct index {
	id_t last;
	int cap;
	int cnt;
	struct rwlock lock;
	struct slot *slot;
};

struct index *index_new(void) {
	struct index *idx = (struct index *)malloc(sizeof *idx);
	if (!idx) {
		return 0;
	}
	idx->last = 0;
	idx->cap = INDEX_SLOT;
	idx->cnt = 0;
	rwlock_init(&idx->lock);
	idx->slot = (struct slot *)calloc(idx->cap, sizeof(struct slot));
	if (!idx->slot) {
		free(idx);
		return 0;
	}
	return idx;
}

void index_free(struct index *idx) {
	if (idx) {
		free(idx->slot);
		free(idx);
	}
}

static struct index *_index_expand(struct index *idx) {
	int i, cap = idx->cap;
	struct slot *nslot = (struct slot *)calloc(cap * 2, sizeof(struct slot));
	if (!nslot) return 0;
	for (i = 0; i < cap; i++) {
		struct slot *os = &idx->slot[i];
		struct slot *ns = &nslot[os->id & (cap * 2 - 1)];
		*ns = *os;
	}
	free(idx->slot);
	idx->slot = nslot;
	idx->cap = cap * 2;
	return idx;
}

id_t index_regist(struct index *idx, void *ud) {
	if (!ud) return 0;
	rwlock_wlock(&idx->lock);
	if (idx->cnt >= idx->cap * 3 / 4) {
		if (!_index_expand(idx)) {
			rwlock_wunlock(&idx->lock);
			return 0;
		}
	}
	for (;;) {
		struct slot *slot;
		id_t id = ++idx->last;
		if (id == 0) {
			id = ++idx->last;
		}
		slot = &idx->slot[HASH(id)];
		if (slot->id) continue;
		slot->id = id;
		slot->ref = 1;
		slot->ud = ud;
		++idx->cnt;
		rwlock_wunlock(&idx->lock);
		return id;
	}
}

void *index_grab(struct index *idx, id_t id) {
	struct slot *slot;
	void *ud;
	if (id == 0) return 0;
	rwlock_rlock(&idx->lock);
	slot = &idx->slot[HASH(id)];
	if (slot->id != id) {
		rwlock_runlock(&idx->lock);
		return 0;
	}
	atom_inc(&slot->ref);
	ud = slot->ud;
	rwlock_runlock(&idx->lock);
	return ud;
}

void *index_release(struct index *idx, id_t id) {
	struct slot *slot;
	void *ud = 0;
	if (id == 0) return 0;
	rwlock_rlock(&idx->lock);
	slot = &idx->slot[HASH(id)];
	if (slot->id != id) {
		rwlock_runlock(&idx->lock);
		return 0;
	}
	if (atom_dec(&slot->ref) <= 0) ud = slot->ud;
	rwlock_runlock(&idx->lock);

	if (ud > 0) {
		rwlock_wlock(&idx->lock);
		slot = &idx->slot[HASH(id)];
		if (slot->id != id) {
			rwlock_wunlock(&idx->lock);
			return 0;
		}
		if (slot->ref > 0) {
			rwlock_wunlock(&idx->lock);
			return 0;
		}
		ud = slot->ud;
		slot->id = 0;
		--idx->cnt;
		rwlock_wunlock(&idx->lock);
		return ud;
	}
	return 0;
}

int index_list(struct index *idx, int n, id_t *list) {
	int i, cnt = 0;
	rwlock_rlock(&idx->lock);
	if (list) {
		for (i = 0; cnt < n && i < idx->cap; i++) {
			struct slot *slot = &idx->slot[i];
			if (slot->id == 0) continue;
			list[cnt] = slot->id;
			++cnt;
		}
	}
	cnt = idx->cnt;
	rwlock_runlock(&idx->lock);
	return cnt;
}
