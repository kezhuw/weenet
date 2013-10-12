#ifndef __WEENET_COMPAT_H_
#define __WEENET_COMPAT_H_

#if defined(__APPLE__)

#include <stddef.h>

void* memrchr(const void *s, int c, size_t n);

#endif

#endif
