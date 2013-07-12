default : debug

debug : CFLAGS += -g -O0 -fsanitize=address -fno-omit-frame-pointer
debug : normal

release : CFLAGS += -g -O2
release : normal

release0 : CFLAGS += -O3
release0 : normal

CC = clang
CFLAGS = -std=c99 -Wall -Wextra -Wconversion -shared -fPIC

SERVICES = gate agent client

LIBS = $(addsuffix .so, $(SERVICES))

normal : $(LIBS)

$(LIBS) : %.so : %.c
	$(CC) $(CFLAGS) $^ -o $@ -I../../src/

clean :
	-rm $(LIBS)

.PHONY : default normal debug release release0 clean
