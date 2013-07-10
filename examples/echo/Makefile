default : all

CC = clang
CFLAGS = -std=c99 -Wall -Wextra -g -shared -fPIC

SERVICES = gate agent client

LIBS = $(addsuffix .so, $(SERVICES))

all : $(LIBS)

$(LIBS) : %.so : %.c
	$(CC) $(CFLAGS) $^ -o $@ -I../../src/

clean :
	-rm $(LIBS)

.PHONY : default all
