#ifndef __WEENET_UTILS_H_
#define __WEENET_UTILS_H_

#include <string.h>

#define memzero(ptr, len)	memset((ptr), 0, (len))

#define nelem(arr)		(sizeof(arr)/sizeof((arr)[0]))

#endif
