#include "compat.h"

#if defined(__APPLE__)

void *
memrchr(const void *s, int c, size_t n) {
	const unsigned char *i = (unsigned char *)s + n;
	while (i-- > (unsigned char *)s) {
		if (*i == c) {
			return (void*)i;
		}
	}
	return NULL;
}

#endif
