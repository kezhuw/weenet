#ifndef __WEENET_SLAB_H_
#define __WEENET_SLAB_H_

#include <stddef.h>

struct slab;

struct slab *slab_new(size_t nitem, size_t isize);
void slab_delete(struct slab *);

void *slab_retain(struct slab *);
void slab_release(struct slab *sa, void *ptr);

#endif
