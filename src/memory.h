#ifndef __WEENET_MEMORY_H_
#define __WEENET_MEMORY_H_

#include "config.h"
#include <stddef.h>
#include <stdint.h>

#ifdef WEENET_CUSTOM_MALLOC

void *wmalloc(size_t size);
void *wcalloc(size_t size);	// zero-initialized memory
void *wrealloc(void *ptr, size_t size);
void wfree(void *ptr);

char *wstrdup(const char *str);
char *wstrldup(const char *str, size_t len);

// Query memory usage status.
//
// Store number of allocations in *blocks, return total memory allocated in bytes.
int64_t wstatus(int64_t *blocks);

#else

#include <stdlib.h>
#include <string.h>

#define wmalloc(size)		malloc(size)
#define wcalloc(size)		calloc(1, (size))
#define wrealloc(ptr, size)	realloc((ptr), (size))
#define wfree(ptr)		free(ptr)

#define wstrdup(str)		strdup(str)
#define wstrldup(str, len)	strndup((str), (len))	// ??? link ?

inline static int64_t
wstatus(int64_t *blocks) {
	*blocks = 1;
	return 0;
}

#endif

#endif
