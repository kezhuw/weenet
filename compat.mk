PLATFORM = $(shell uname -s)
PLAT = none

ifeq ($(PLATFORM), Linux)
	MACROS += -D_GNU_SOURCE -D_POSIX_SOURCE -D_BSD_SOURCE
	LDFLAGS += -lm
	PLAT = linux
endif

ifeq ($(PLATFORM), Darwin)
	SHARED = -fPIC -dynamiclib -Wl,-undefined,dynamic_lookup
	PLAT = macosx
else
	SHARED = -fPIC -shared
endif

ifeq ($(PLATFORM), FreeBSD)
	PLAT = freebsd
else
	LDFLAGS += -ldl
endif
