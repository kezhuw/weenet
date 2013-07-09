#ifndef __WEENET_TYPES_H_
#define __WEENET_TYPES_H_

#include <stdint.h>

typedef uint8_t byte_t;
typedef int64_t intreg_t;
typedef uint64_t uintreg_t;

typedef int (*func_t)();

typedef uint32_t process_t;
typedef uint32_t session_t;
typedef uint32_t monitor_t;

enum { SESSION_ZERO = 0 };
enum { PROCESS_ZERO = 0 };
enum { MONITOR_ZERO = 0 };

#endif
