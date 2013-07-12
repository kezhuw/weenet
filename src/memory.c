#include "config.h"

#ifdef WEENET_CUSTOM_MALLOC
#include "memory.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int64_t used_memory;
static int64_t numb_blocks;

#define memory_add(n)	__sync_add_and_fetch(&used_memory, (int64_t)n)
#define memory_sub(n)	__sync_sub_and_fetch(&used_memory, (int64_t)n)

#define blocks_add()	__sync_add_and_fetch(&numb_blocks, 1)
#define blocks_sub()	__sync_sub_and_fetch(&numb_blocks, 1)

#define RETPTR(ptr)	((void*)((char*)ptr + sizeof(size_t)))
#define REALPTR(ptr)	((void*)((char*)ptr - sizeof(size_t)))
#define REALSIZE(size)	(size + sizeof(size_t))

int64_t
wstatus(int64_t *blocks) {
	*blocks = numb_blocks;
	return used_memory;
}

void *
wmalloc(size_t size) {
	void *ptr = malloc(REALSIZE(size));
	*((size_t*)ptr) = size;
	memory_add(size);
	blocks_add();
	return RETPTR(ptr);
}

void *
wcalloc(size_t size) {
	void *ptr = calloc(1, REALSIZE(size));
	*((size_t*)ptr) = size;
	memory_add(size);
	blocks_add();
	return RETPTR(ptr);
}

void
wfree(void *ptr) {
	if (ptr == NULL) return;
	void *realptr = REALPTR(ptr);
	size_t size = *((size_t*)realptr);
	memory_sub(size);
	blocks_sub();
	free(realptr);
}

void *
wrealloc(void *ptr, size_t size) {
	if (ptr == NULL) return wmalloc(size);
	void *realptr = REALPTR(ptr);
	size_t oldsize = *((size_t*)realptr);
	void *newptr = realloc(realptr, REALSIZE(size));
	*((size_t*)newptr) = size;
	memory_sub(oldsize);
	memory_add(size);
	return RETPTR(newptr);
}

char *
wstrldup(const char *str, size_t len) {
	char *ptr = wmalloc(len+1);
	memcpy(ptr, str, len);
	ptr[len] = '\0';
	return ptr;
}

char *
wstrdup(const char *str) {
	return wstrldup(str, strlen(str));
}
#endif
