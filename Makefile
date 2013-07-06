default : all

CC = clang
CFLAGS = -Wall -Wextra
LDFLAGS = -lpthread -llua -ldl

BUILD = build

SERVICES = listener logger #lua

#self = $1
define SERVICE_SRC
$(addprefix src/services/, $(addprefix $1/, $(addsuffix .c, $1)))
endef

define SERVICE_LIB
$(addprefix $(BUILD)/services/, $(addsuffix .so, $1))
endef

KERNEL := $(shell uname -s)

SRCS = atom.c event.c logger.c pipe.c memory.c process.c service.c slab.c main.c schedule.c timer.c

$(BUILD)/weenet: $(addprefix src/, $(SRCS)) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

%.c : %.h
	@touch $@

# build every services
.SECONDEXPANSION:
$(addprefix $(BUILD)/services/, $(addsuffix .so, $(SERVICES))) : build/services/%.so : $$(call SERVICE_SRC, %)
	$(CC) $(CFLAGS) -shared -fPIC $^ -o $@ -Isrc

$(BUILD) : $(BUILD)/services/
$(BUILD)/services/ :
	@mkdir -p $@

all : $(addprefix $(BUILD)/, weenet $(addsuffix .so, $(addprefix services/, $(SERVICES))))

clean :
	rm -rf $(BUILD)

.PHONY : all clean
