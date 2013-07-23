#include "slab.h"
#include "types.h"
#include "atomic.h"
#include "memory.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

struct link {
	struct link *link;
};

struct block {
	struct block *next;
	byte_t bytes[];
};

struct slab {
	size_t blocksize;
	size_t piecesize;

	size_t nblocks;
	int64_t allocnum;

	struct link * volatile freelist;
	int64_t freelock;

	byte_t *allocpos;
	byte_t *sentinel;
	struct block *blocklast;
	int64_t blocklock;

	struct block blockfirst;
};

struct slab *
slab_new(size_t npieces, size_t piecesize) {
	assert(piecesize >= sizeof(void *));
	assert(npieces > 0);
	size_t blocksize = piecesize * npieces;
	struct slab *sa = wmalloc(sizeof(*sa) + blocksize);

	sa->blocksize = blocksize;
	sa->piecesize = piecesize;

	sa->nblocks = 1;
	sa->allocnum = 0;

	sa->freelist = NULL;
	sa->freelock = 0;

	sa->allocpos = sa->blockfirst.bytes;
	sa->sentinel = sa->blockfirst.bytes + blocksize;
	sa->blockfirst.next = NULL;
	sa->blocklast = &sa->blockfirst;
	sa->blocklock = 0;

	return sa;
}

void
slab_delete(struct slab *sa) {
	assert(sa->allocnum == 0);
	struct block *b = sa->blockfirst.next;
	size_t nblocks = sa->nblocks - 1;
	while (b != NULL) {
		struct block *next = b->next;
		wfree(b);
		b = next;
		--nblocks;
	}
	assert(nblocks == 0);
	wfree(sa);
}

void *
slab_retain(struct slab *sa) {
	weenet_atomic_inc(&sa->allocnum);

	weenet_atomic_lock(&sa->freelock);
	if (sa->freelist != NULL) {
		for (;;) {
			// Other piece can be inserted before 'freelist' in
			// slab_release(), but no pieces will be pop out
			// from 'freelist'.
			//
			// So, value of 'freelist' may changed, but the saved
			// 'li' and its 'link' field are consistent.
			struct link *li = sa->freelist;
			if (weenet_atomic_cas(&sa->freelist, li, li->link)) {
				weenet_atomic_unlock(&sa->freelock);
				return li;
			}
		}
	}
	weenet_atomic_unlock(&sa->freelock);

	weenet_atomic_lock(&sa->blocklock);
	if (sa->allocpos == sa->sentinel) {
		struct block *b = wmalloc(sizeof(*b) + sa->blocksize);
		b->next = NULL;
		sa->nblocks += 1;
		sa->allocpos = b->bytes;
		sa->sentinel = b->bytes + sa->blocksize;
		sa->blocklast->next = b;
		sa->blocklast = b;
	}
	void *ptr = sa->allocpos;
	sa->allocpos += sa->piecesize;
	weenet_atomic_unlock(&sa->blocklock);
	return ptr;
}

void
slab_release(struct slab *sa, void *ptr) {
	struct link *li = ptr;
	for (;;) {
		struct link *frees = sa->freelist;
		li->link = frees;
		// It's ok for situations that memory 'freelist' pointed
		// is retained and released again.
		//
		// When 'freelist' == frees, frees is the head of current
		// freelist.
		if (weenet_atomic_cas(&sa->freelist, frees, li)) {
			break;
		}
	}

	weenet_atomic_dec(&sa->allocnum);
}
