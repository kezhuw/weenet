NAME = weenet

default : debug

debug : CFLAGS += -g
debug : normal

asan : CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan : debug

release : CFLAGS += -g -O2
release : normal

release0 : CFLAGS += -O3
release0 : normal

CC = clang
CFLAGS = -std=c99 -I$(LUA_INC) -Wall -Wextra -Wconversion
LDFLAGS = -lpthread -rdynamic

include compat.mk

CFLAGS += $(MACROS)

PREFIX = /usr/local
INSTALL_ETC = $(PREFIX)/etc
INSTALL_BIN = $(PREFIX)/bin
INSTALL_LIB = $(PREFIX)/lib
INSTALL_INC = $(PREFIX)/include
INSTALL_SERVICES_DIR = $(INSTALL_LIB)/$(NAME)/services
INSTALL_INCLUDES_DIR = $(INSTALL_INC)/$(NAME)
INSTALL = install -v

BUILD = build
BUILD_SERVICES_DIR = $(BUILD)/services

SERVICES = listener logger lua
WEENET_BIN = $(BUILD)/$(NAME)
WEENET_CONF = etc/weenet.conf
SERVICES_DIR = $(BUILD)/services
SERVICES_BIN = $(addprefix $(SERVICES_DIR)/, $(addsuffix .so, $(SERVICES)))

INCS = weenet.h atom.h atomic.h compat.h event.h timer.h types.h logger.h memory.h process.h service.h
HEADERS = $(addprefix src/, $(INCS))

define SERVICE_SRC
$(addprefix src/services/, $(addprefix $1/, $(addsuffix .c, $1)))
endef

LUA_DIR := deps/lua
LUA_INC := deps/lua/src
LUA_LIB := deps/lua/src/liblua.a

$(LUA_LIB) :
	$(MAKE) -C $(LUA_DIR) 'CC=$(CC)' $(PLAT)

SRCS = atom.c compat.c event.c logger.c pipe.c memory.c process.c service.c slab.c main.c schedule.c timer.c socket_buffer.c

$(WEENET_BIN) : $(addprefix src/, $(SRCS)) $(LUA_LIB) | $(BUILD)
	@echo "Building weenet ..."
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ -Isrc
	@echo "Done"
	@echo

LUA_CPATH = src/lualib/c
LUA_CLIBS = socket
LUA_CBINS = $(addprefix $(LUA_CPATH)/, $(addsuffix .so, $(LUA_CLIBS)))

$(LUA_CBINS) : $(LUA_CPATH)/%.so : $(LUA_CPATH)/%.c
	@echo "Building lua c lib: $*"
	$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Isrc
	@echo "Done"
	@echo

%.c : %.h
	@touch $@

# build every services
.SECONDEXPANSION:
$(SERVICES_BIN) : build/services/%.so : $$(call SERVICE_SRC, %)
	@echo "Building service: $*"
	$(CC) $(CFLAGS) $(SHARED) $^ -o $@ -Isrc
	@echo "Done"
	@echo

$(BUILD) : $(BUILD_SERVICES_DIR)

$(BUILD_SERVICES_DIR) $(INSTALL_SERVICES_DIR) $(INSTALL_INCLUDES_DIR):
	@mkdir -p $@

normal : $(WEENET_BIN) $(SERVICES_BIN) $(LUA_CBINS)

install : $(INSTALL_SERVICES_DIR) $(INSTALL_INCLUDES_DIR)
	$(INSTALL) $(WEENET_BIN) $(INSTALL_BIN)
	$(INSTALL) $(WEENET_CONF) $(INSTALL_ETC)
	$(INSTALL) $(SERVICES_BIN) $(INSTALL_SERVICES_DIR)
	$(INSTALL) $(HEADERS) $(INSTALL_INCLUDES_DIR)
	@echo "#include \"weenet/weenet.h\"" > $(INSTALL_INC)/weenet.h

clean :
	rm -rf $(BUILD) $(LUA_CBINS)
	$(MAKE) -C $(LUA_DIR) $@

.PHONY : default normal debug release release0 clean
