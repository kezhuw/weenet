#ifndef __WEENET_ATOMIC_H_
#define __WEENET_ATOMIC_H_

//#include "types.h"
//
//typedef intreg_t weenet_atomic_t;

#define weenet_atomic_lock(l)		while (__sync_lock_test_and_set(l, 1)) {}
#define weenet_atomic_unlock(l)		__sync_lock_release(l)
#define weenet_atomic_trylock(l)	(__sync_lock_test_and_set(l, 1) == 0)
#define weenet_atomic_locked(l)		__sync_bool_compare_and_swap(l, 1, 1)

#define weenet_atomic_get(p)		(*(p))
#define weenet_atomic_add(p, v)		__sync_add_and_fetch((p), (v))
#define weenet_atomic_sub(p, v)		__sync_sub_and_fetch((p), (v))
#define weenet_atomic_cas(p, e, v)	__sync_bool_compare_and_swap((p), (e), (v))
#define weenet_atomic_inc(p)		weenet_atomic_add(p, 1)
#define weenet_atomic_dec(p)		weenet_atomic_sub(p, 1)

#define weenet_atomic_sync()		__sync_synchronize()

#endif
