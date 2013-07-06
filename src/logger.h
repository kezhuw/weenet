#ifndef __WEENET_LOGGER_H_
#define __WEENET_LOGGER_H_

int weenet_init_logger(const char *dir);

void weenet_logger_printf(const char *fmt, ...);
void weenet_logger_errorf(const char *fmt, ...);
void weenet_logger_fatalf(const char *fmt, ...);

#endif
