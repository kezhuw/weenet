PLATFORM = $(shell uname -s)

ifeq ($(PLATFORM), Linux)
	MACROS += -D_GNU_SOURCE -D_POSIX_SOURCE -D_BSD_SOURCE
endif

ifeq ($(PLATFORM), Darwin)
	SHARED = -fPIC -dynamiclib -Wl,-undefined,dynamic_lookup
else
	SHARED = -fPIC -shared
endif
