include ../../compat.mk

default : debug

debug : CFLAGS += -g -O0
debug : normal

asan : CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan : debug

release : CFLAGS += -g -O2
release : normal

release0 : CFLAGS += -O3
release0 : normal

CC = clang
CFLAGS = -std=c99 -Wall -Wextra -Wconversion $(MACROS) $(SHARED)

SERVICES = gate agent client

LIBS = $(addsuffix .so, $(SERVICES))

normal : $(LIBS)

$(LIBS) : %.so : %.c
	$(CC) $(CFLAGS) $^ -o $@ -I../../src/

clean :
	-rm $(LIBS)

.PHONY : default normal debug release release0 clean
