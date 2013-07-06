#ifndef __WEENET_MEMORY_H_
#define __WEENET_MEMORY_H_

#include <stddef.h>
#include <stdint.h>

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

#endif
