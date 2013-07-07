#ifndef __WEENET_LOGGER_H_
#define __WEENET_LOGGER_H_

#include <stddef.h>

int weenet_init_logger(const char *dir, size_t limit);

void weenet_logger_printf(const char *fmt, ...);
void weenet_logger_errorf(const char *fmt, ...);
void weenet_logger_fatalf(const char *fmt, ...);

#endif
