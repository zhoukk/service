#ifndef _lock_h_
#define _lock_h_

#define atom_cas(ptr, oval, nval) __sync_bool_compare_and_swap(ptr, oval, nval)
#define atom_inc(ptr) __sync_add_and_fetch(ptr, 1)
#define atom_dec(ptr) __sync_sub_and_fetch(ptr, 1)
#define atom_and(ptr, n) __sync_and_and_fetch(ptr, n)
#define atom_add(ptr, n) __sync_add_and_fetch(ptr, n)
#define atom_sub(ptr, n) __sync_sub_and_fetch(ptr, n)
#define atom_sync() __sync_synchronize()
#define atom_spinlock(ptr) while (__sync_lock_test_and_set(ptr, 1)) {}
#define atom_spinunlock(ptr) __sync_lock_release(ptr)


#ifdef USE_PTHREAD_LOCK

#include <pthread.h>

struct rwlock {
	pthread_rwlock_t lock;
};

static inline void rwlock_init(struct rwlock *lock) {
	pthread_rwlock_init(&lock->lock, 0);
}

static inline void rwlock_rlock(struct rwlock *lock) {
	pthread_rwlock_rdlock(&lock->lock);
}

static inline void rwlock_runlock(struct rwlock *lock) {
	pthread_rwlock_unlock(&lock->lock);
}

static inline void rwlock_wlock(struct rwlock *lock) {
	pthread_rwlock_wrlock(&lock->lock);
}

static inline void rwlock_wunlock(struct rwlock *lock) {
	pthread_rwlock_unlock(&lock->lock);
}

struct spinlock {
	pthread_mutex_t lock;
};

static inline void spinlock_init(struct spinlock *lock) {
	pthread_mutex_init(&lock->lock, 0);
}

static inline void spinlock_lock(struct spinlock *lock) {
	pthread_mutex_lock(&lock->lock);
}

static inline void spinlock_unlock(struct spinlock *lock) {
	pthread_mutex_unlock(&lock->lock);
}

static inline void spinlock_unit(struct spinlock *lock) {
	pthread_mutex_destroy(&lock->lock);
}


#else

struct rwlock {
	int read;
	int write;
};

static inline void rwlock_init(struct rwlock *lock) {
	lock->read = 0;
	lock->write = 0;
}

static inline void rwlock_rlock(struct rwlock *lock) {
	for (;;) {
		while (lock->write) atom_sync();
		atom_inc(&lock->read);
		if (lock->write) atom_dec(&lock->read);
		else break;
	}
}

static inline void rwlock_runlock(struct rwlock *lock) {
	atom_dec(&lock->read);
}

static inline void rwlock_wlock(struct rwlock *lock) {
	atom_spinlock(&lock->write);
	while (lock->read) atom_sync();
}

static inline void rwlock_wunlock(struct rwlock *lock) {
	atom_spinunlock(&lock->write);
}


struct spinlock {
	int lock;
};

static inline void spinlock_init(struct spinlock *lock) {
	lock->lock = 0;
}

static inline void spinlock_lock(struct spinlock *lock) {
	atom_spinlock(&lock->lock);
}

static inline void spinlock_unlock(struct spinlock *lock) {
	atom_spinunlock(&lock->lock);
}

static inline void spinlock_unit(struct spinlock *lock) {
	(void)lock;
}

#endif // USE_PTHREAD_LOCK

#endif // _lock_h_