#ifndef __WEENET_CONFIG_H_
#define __WEENET_CONFIG_H_

#define WEENET_DEFAULT_SERVICE_PATH	"/usr/lib/weenet/services/?.so;/usr/share/weenet/services/?.so"

#define WEENET_DEFAULT_CONFIG_FILE	"/etc/weenet.conf"

#define WEENET_DEFAULT_LOGGER_DIR	"/var/log/weenet"

#define WEENET_DEFAULT_THREADS		8

#define WEENET_CUSTOM_MALLOC

#define WEENET_MAX_OPEN_FILES		10000

#endif
